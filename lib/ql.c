/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2012 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "groonga_in.h"
#include <string.h>
#include "ql.h"
#include "db.h"
#include "pat.h"
#include "snip.h"

#define DB_OBJ(obj) ((grn_db_obj *)obj)
#define RVALUE(obj) (grn_ctx_at(ctx, (obj)->u.o.id))
#define INTERN2(s,l) (grn_ql_mk_symbol(ctx, s, l))

static grn_cell *ha_object(grn_ctx *ctx, grn_cell *args, grn_ql_co *co);
static grn_cell *ha_snip(grn_ctx *ctx, grn_cell *args, grn_ql_co *co);
static grn_cell *ha_sections(grn_ctx *ctx, grn_cell *args, grn_ql_co *co);

#define STRBUF_SIZE (GRN_TABLE_MAX_KEY_SIZE + 1)

static grn_rc
obj2str(grn_cell *o, char *buf, uint16_t *size)
{
  if (SYMBOLP(o)) {
    const char *r = _grn_hash_strkey_by_val(o, size);
    if (r && *r == ':') {
      r++;
      (*size)--;
    }
    memcpy(buf, r, *size);
    buf[*size] = '\0';
  } else if (o->header.type == GRN_CELL_STR) {
    if (STRSIZE(o) > GRN_TABLE_MAX_KEY_SIZE) {
      memcpy(buf, STRVALUE(o), GRN_TABLE_MAX_KEY_SIZE);
      buf[GRN_TABLE_MAX_KEY_SIZE] = '\0';
      *size = GRN_TABLE_MAX_KEY_SIZE + 1;
    } else {
      *size = STRSIZE(o);
      memcpy(buf, STRVALUE(o), *size);
      buf[*size] = '\0';
    }
  } else if (o->header.type == GRN_CELL_INT) {
    char *rest;
    grn_lltoa(IVALUE(o), buf, buf + GRN_TABLE_MAX_KEY_SIZE, &rest);
    *size = rest - buf;
    *rest = '\0';
  } else {
    return GRN_INVALID_ARGUMENT;
  }
  return GRN_SUCCESS;
}

static void
obj_obj_bind(grn_cell *obj, grn_id domain, grn_id self)
{
  obj->header.type = GRN_CELL_OBJECT;
  obj->header.impl_flags = GRN_CELL_NATIVE;
  obj->u.o.id = self;
  obj->header.domain = domain;
  obj->u.o.func = ha_object;
}

static grn_cell *
obj2cell(grn_ctx *ctx, grn_obj *obj, grn_cell *cell)
{
  if (GRN_DB_OBJP(obj)) {
    grn_id id = DB_OBJ(obj)->id;
    if (id & GRN_OBJ_TMP_OBJECT) {
      grn_tmp_db_obj *tmp_obj;
      if ((tmp_obj = _grn_array_get_value(ctx, ctx->impl->values,
                                          id & ~GRN_OBJ_TMP_OBJECT))) {
        return &tmp_obj->cell;
      } else {
        return F;
      }
    } else {
      char buf[STRBUF_SIZE];
      int len = grn_obj_name(ctx, obj, buf, STRBUF_SIZE);
      return INTERN2(buf, len);
    }
  }
  if (!cell) { if (!(cell = grn_cell_new(ctx))) { return F; } }
  switch (obj->header.type) {
  case GRN_BULK :
    {
      void *v = GRN_BULK_HEAD(obj);
      grn_id rid = obj->header.domain;
      grn_obj *range = grn_ctx_at(ctx, rid);
      if (v) {
        if (range && range->header.type == GRN_TYPE) {
          switch (rid) {
          case GRN_DB_INT32 :
            SETINT(cell, *((int32_t *)v));
            break;
          case GRN_DB_UINT32 :
            SETINT(cell, *((uint32_t *)v));
            break;
          case GRN_DB_INT64 :
            SETINT(cell, *((int64_t *)v));
            break;
          case GRN_DB_FLOAT :
            SETFLOAT(cell, *((double *)v));
            break;
          case GRN_DB_TIME :
            SETTIME(cell, v);
            break;
          default :
            {
              uint32_t size = GRN_BULK_VSIZE(obj);
              void *value = GRN_MALLOC(size);
              if (!value) { return F; }
              cell->header.impl_flags |= GRN_OBJ_ALLOCATED;
              memcpy(value, v, size);
              SETBULK(cell, value, size);
            }
            break;
          }
        } else {
          obj_obj_bind(cell, rid, *((grn_id *)v));
        }
      }
    }
    break;
  case GRN_UVECTOR :
    {
      grn_obj *sections = grn_obj_graft(ctx, obj);
      if (sections) {
        cell->header.type = GRN_UVECTOR;
        cell->header.impl_flags |= GRN_OBJ_ALLOCATED|GRN_CELL_NATIVE;
        cell->u.p.value = sections;
        cell->u.p.func = ha_sections;
      }
    }
    break;
  case GRN_VECTOR :
    {
      grn_obj *sections = grn_obj_graft(ctx, obj);
      if (sections) {
        cell->header.type = GRN_VECTOR;
        cell->header.impl_flags |= GRN_OBJ_ALLOCATED|GRN_CELL_NATIVE;
        cell->u.p.value = sections;
        cell->u.p.func = ha_sections;
      }
    }
    break;
  }
  return cell;
}

static int
keywordp(grn_cell *x)
{
  uint16_t name_size;
  const char *name;
  return SYMBOLP(x)
    && (name = _grn_hash_strkey_by_val(x, &name_size))
    && name_size
    && *name == ':';
}

static int
symbolp(grn_cell *x)
{
  uint16_t name_size;
  const char *name;
  return SYMBOLP(x)
    && (name = _grn_hash_strkey_by_val(x, &name_size))
    && (!name_size || *name != ':');
}

static int
descp(grn_cell *x)
{
  uint16_t name_size;
  const char *name;
  return SYMBOLP(x)
    && (name = _grn_hash_strkey_by_val(x, &name_size))
    && (name_size > 1)
    && (*name == ':')
    && (name[1] == 'd');
}

/* column_exp */

typedef struct {
  grn_cell *expr;
  grn_table_sort_key *keys;
  grn_cell **cells;
  int n_keys;
  int n_applys;
} column_exp;

static grn_obj *
get_accessor(grn_ctx *ctx, grn_obj *table, grn_cell *e)
{
  uint16_t msg_size;
  char msg[STRBUF_SIZE];
  obj2str(e, msg, &msg_size);
  return grn_obj_column(ctx, table, msg, msg_size);
}

#define COLUMN_EXPP(x,ce) ((x) == (ce)->cells[(ce)->n_keys])

static grn_cell *
column_exp_build_(grn_ctx *ctx, grn_obj *table, grn_cell *e, column_exp *ce, grn_cell *parameter)
{
  if (PAIRP(e)) {
    grn_cell *x, *r, *l = NIL, **d;
    POP(x, e);
    if (x == parameter && keywordp(CAR(e))) {
      if (ce->keys[ce->n_keys].key) { ce->n_keys++; }
      ce->keys[ce->n_keys].key = get_accessor(ctx, table, CAR(e));
      d = &ce->cells[ce->n_keys];
      if (!*d) { *d = grn_cell_new(ctx); }
      r = *d;
    } else {
      r = column_exp_build_(ctx, table, x, ce, parameter);
      if (COLUMN_EXPP(r, ce) && keywordp(CAR(e))) {
        get_accessor(ctx, ce->keys[ce->n_keys].key, CAR(e));
      } else {
        r = CONS(r, NIL);
        d = &CDR(r);
        while (PAIRP(e)) {
          POP(x, e);
          if (COLUMN_EXPP(l, ce) && descp(x)) {
            ce->keys[ce->n_keys].flags |= GRN_TABLE_SORT_DESC;
          }
          l = column_exp_build_(ctx, table, x, ce, parameter);
          *d = CONS(l, NIL);
          d = &CDR(*d);
        }
      }
    }
    return r;
  } else {
    return e;
  }
}

static void
column_exp_build(grn_ctx *ctx, grn_obj *table, grn_cell *e, column_exp *ce, grn_cell *parameter)
{
  grn_cell *x, *r = e, *l = NIL, **d = &r;
  ce->n_keys = 0;
  while (PAIRP(e)) {
    POP(x, e);
    if (COLUMN_EXPP(l, ce) && descp(x)) {
      ce->keys[ce->n_keys].flags |= GRN_TABLE_SORT_DESC;
    }
    l = column_exp_build_(ctx, table, x, ce, parameter);
    *d = CONS(l, NIL);
    d = &CDR(*d);
  }
  if (ce->keys[ce->n_keys].key) { ce->n_keys++; }
  ce->expr = r;
  grn_ql_obj_mark(ctx, ce->expr);
}

#define EVAL_BY_FUNCALLP(ce) \
 ((ce)->n_applys == 1 && PAIRP((ce)->expr) && NATIVE_FUNCP(CAR((ce)->expr)))

static void
column_exp_check(grn_cell *e, int *ns, int *ne, grn_cell *parameter)
{
  if (PAIRP(e)) {
    grn_cell *x;
    POP(x, e);
    if (x == parameter) {
      (*ns)++;
    } else if (NATIVE_FUNCP(x)) {
      (*ne)++;
    } else {
      column_exp_check(x, ns, ne, parameter);
    }
    while (PAIRP(e)) {
      POP(x, e);
      column_exp_check(x, ns, ne, parameter);
    }
  } else {
    if (symbolp(e)) { (*ne)++; }
  }
}

static column_exp *
column_exp_open(grn_ctx *ctx, grn_obj *table, grn_cell *expr, grn_cell *parameter)
{
  column_exp *ce = GRN_CALLOC(sizeof(column_exp));
  if (ce) {
    column_exp_check(expr, &ce->n_keys, &ce->n_applys, parameter);
    if (ce->n_keys) {
      if (!(ce->keys = GRN_CALLOC(sizeof(grn_table_sort_key) * ce->n_keys))) {
        GRN_FREE(ce);
        return NULL;
      }
      if (!(ce->cells = GRN_CALLOC(sizeof(grn_cell *) * ce->n_keys))) {
        GRN_FREE(ce->keys);
        GRN_FREE(ce);
        return NULL;
      }
      column_exp_build(ctx, table, expr, ce, parameter);
    } else {
      ce->expr = expr;
    }
  }
  return ce;
}

static grn_rc
column_exp_exec(grn_ctx *ctx, column_exp *ce, grn_id id)
{
  int i;
  grn_obj v, *vp;
  GRN_TEXT_INIT(&v, 0);
  for (i = 0; i < ce->n_keys; i++) {
    grn_cell *c = ce->cells[i];
    GRN_BULK_REWIND(&v);
    vp = grn_obj_get_value(ctx, ce->keys[i].key, id, &v);
    grn_cell_clear(ctx, c);
    obj2cell(ctx, vp, c);
  }
  grn_obj_close(ctx, &v);
  return GRN_SUCCESS;
}

static grn_rc
column_exp_close(grn_ctx *ctx, column_exp *ce)
{
  int i;
  grn_ql_obj_unmark(ctx, ce->expr);
  for (i = 0; i < ce->n_keys; i++) {
    grn_obj_unlink(ctx, ce->keys[i].key);
  }
  if (ce->keys) { GRN_FREE(ce->keys); }
  if (ce->cells) { GRN_FREE(ce->cells); }
  GRN_FREE(ce);
  return GRN_SUCCESS;
}

static grn_cell *
ha_void(grn_ctx *ctx, grn_cell *args, grn_ql_co *co)
{
  if (!ctx->impl->code) { return F; }
  return ctx->impl->code;
}

grn_cell *
grn_ql_obj_new(grn_ctx *ctx, grn_id domain, grn_id self)
{
  grn_cell *o;
  GRN_CELL_NEW(ctx, o);
  obj_obj_bind(o, domain, self);
  return o;
}

static grn_cell *
get_cell(grn_ctx *ctx, grn_obj *db_obj)
{
  if (db_obj) {
    grn_id id = DB_OBJ(db_obj)->id;
    if (id & GRN_OBJ_TMP_OBJECT) {
      grn_tmp_db_obj *tmp_obj;
      if ((tmp_obj = _grn_array_get_value(ctx, ctx->impl->values,
                                          id & ~GRN_OBJ_TMP_OBJECT))) {
        return &tmp_obj->cell;
      } else {
        return F;
      }
    } else {
      char buf[STRBUF_SIZE];
      int len = grn_obj_name(ctx, db_obj, buf, STRBUF_SIZE);
      return INTERN2(buf, len);
    }
  } else {
    return F;
  }
}

static grn_obj *
get_obj(grn_ctx *ctx, grn_cell *cell)
{
  return GRN_DB_OBJP(cell) ? grn_ctx_at(ctx, cell->u.o.id) : NULL;
}

static grn_obj *
get_domain(grn_ctx *ctx, grn_cell *cell)
{
  return grn_ctx_at(ctx, cell->header.domain);
}

static void
unesc(grn_ctx *ctx, grn_cell *obj)
{
  char *src, *dest, *end = STRVALUE(obj) + STRSIZE(obj);
  for (src = dest = STRVALUE(obj);;) {
    unsigned int len;
    if (!(len = grn_charlen(ctx, src, end))) { break; }
    if (src[0] == '\\' && src + 1 < end && len == 1) {
      src++;
      switch (*src) {
      case 'n' :
        *dest++ = '\n';
        break;
      case 'r' :
        *dest++ = '\r';
        break;
      case 't' :
        *dest++ = '\t';
        break;
      default :
        *dest++ = *src;
        break;
      }
      src++;
    } else {
      while (len--) { *dest++ = *src++; }
    }
  }
  STRSIZE(obj) = dest - STRVALUE(obj);
}

static grn_cell *
grn_ql_table_add(grn_ctx *ctx, grn_obj *table, const void *key, unsigned int key_size, grn_cell *res)
{
  grn_id id = grn_table_add(ctx, table, key, key_size, NULL);
  if (id) {
    if (!res) { GRN_CELL_NEW(ctx, res); }
    obj_obj_bind(res, DB_OBJ(table)->id, id);
    return res;
  } else {
    return F;
  }
}

static grn_cell *
grn_ql_table_get(grn_ctx *ctx, grn_obj *table, const void *key, unsigned int key_size, grn_cell *res)
{
  grn_id id = grn_table_get(ctx, table, key, key_size);
  if (id) {
    if (!res) { GRN_CELL_NEW(ctx, res); }
    obj_obj_bind(res, DB_OBJ(table)->id, id);
    return res;
  } else {
    return F;
  }
}

static grn_obj *
get_column(grn_ctx *ctx, grn_id tid, char *msg, unsigned int msg_size, grn_id *id)
{
  grn_obj *table = grn_ctx_at(ctx, tid);
  for (;;) {
    grn_obj *domain = grn_ctx_at(ctx, DB_OBJ(table)->header.domain);
    if (!domain || domain->header.type == GRN_TYPE) { break; }
    if (id) {
      if (!grn_table_get_key(ctx, table, *id, id, sizeof(grn_id))) { return NULL; }
    }
    table = domain;
  }
  return grn_obj_column(ctx, table, msg, msg_size);
}

static grn_cell *
table_column(grn_ctx *ctx, grn_id base, char *msg, unsigned int msg_size)
{
  grn_obj *column = get_column(ctx, base, msg, msg_size, NULL);
  return column ? get_cell(ctx, column) : F;
}

static grn_cell *
ha_sections(grn_ctx *ctx, grn_cell *args, grn_ql_co *co)
{
  uint16_t msg_size;
  grn_cell *car, *res;
  char msg[STRBUF_SIZE];
  if (!(res = ctx->impl->code)) { QLERR("invalid receiver"); }
  POP(car, args);
  if (obj2str(car, msg, &msg_size)) { QLERR("invalid argument"); }
  switch (*msg) {
  case ':' :
    switch (msg[1]) {
    case 's' : /* :sexp */
      {
        grn_obj *sections = (grn_obj *)res->u.p.value;
        int n = sections->u.v.n_sections;
        grn_section *vp = &sections->u.v.sections[n];
        const char *head = GRN_BULK_HEAD(sections->u.v.body);
        grn_cell *a = INTERN("@");
        grn_cell *d = INTERN(":dic");
        grn_cell *w = INTERN(":weight");
        grn_cell *v = INTERN(":value");
        res = NIL;
        while ((vp--, n--)) {
          grn_cell *vs = NIL;
          if (vp->weight || vp->domain) {
            vs = CONS(v, CONS(grn_ql_mk_string(ctx, head + vp->offset, vp->length), vs));
            if (vp->weight) {
              grn_cell *nv;
              GRN_CELL_NEW(ctx, nv);
              SETINT(nv, vp->weight);
              vs = CONS(w, CONS(nv, vs));
            }
            if (vp->domain) {
              grn_cell *nv;
              GRN_CELL_NEW(ctx, nv);
              SETINT(nv, vp->domain);
              vs = CONS(d, CONS(nv, vs));
            }
            vs = CONS(a, vs);
          } else {
            vs = grn_ql_mk_string(ctx, head + vp->offset, vp->length);
          }
          res = CONS(vs, res);
        }
      }
      break;
    default :
      QLERR("invalid argument");
    }
    break;
  default :
    QLERR("invalid argument");
  }
  return res;
}

#define QLWARN(...) ERRSET(ctx, GRN_WARN, GRN_INVALID_ARGUMENT, __VA_ARGS__)

#define STR2DBL(str,len,val) do {\
  char *end, buf0[128], *buf = (len) < 128 ? buf0 : GRN_MALLOC((len) + 1);\
  if (buf) {\
    double d;\
    memcpy(buf, (str), (len));\
    buf[len] = '\0';\
    errno = 0;\
    d = strtod(buf, &end);\
    if (!((len) < 128)) { GRN_FREE(buf); }\
    if (!errno && buf + (len) == end) {\
      (val) = d;\
    } else { QLWARN("cast failed"); }\
  } else { QLWARN("buf alloc failed"); }\
} while (0)

static void
list2vector(grn_ctx *ctx, grn_cell *s, grn_obj *v)
{
  grn_cell *car, *key, *value;
  while (PAIRP(s)) {
    POP(car, s);
    if (BULKP(car)) {
      grn_vector_add_element(ctx, v, STRVALUE(car), STRSIZE(car), 0, GRN_ID_NIL);
    } else if (PAIRP(car)) {
      const char *str = NULL;
      grn_id domain = GRN_ID_NIL;
      unsigned int weight = 0, str_len = 0;
      POP(key, car);
      if (key == INTERN("@")) {
        while (PAIRP(car)) {
          POP(key, car);
          POP(value, car);
          if (key == INTERN(":dic")) {
            /* todo */
          } else if (key == INTERN(":weight")) {
            grn_obj2int(ctx, value);
            weight = (uint32_t) IVALUE(value);
          } else if (key == INTERN(":value") && BULKP(value)) {
            str = STRVALUE(value);
            str_len = STRSIZE(value);
          }
        }
        if (str) {
          grn_vector_add_element(ctx, v, str, str_len, weight, domain);
        }
      }
    }
  }
}

static grn_obj *
cell2obj(grn_ctx *ctx, grn_cell *cell, grn_obj *column, grn_obj *obj)
{
  if ((column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) == GRN_OBJ_COLUMN_SCALAR) {
    grn_id rid = grn_obj_get_range(ctx, column);
    grn_obj *range = grn_ctx_at(ctx, rid);
    if (range && range->header.type == GRN_TYPE) {
      switch (rid) {
      case GRN_DB_INT32 :
        {
          int32_t v;
          switch (cell->header.type) {
          case GRN_CELL_STR :
            {
              int64_t iv = grn_atoll(STRVALUE(cell), STRVALUE(cell) + STRSIZE(cell), NULL);
              v = (int32_t) iv;
            }
            break;
          case GRN_CELL_INT :
            v = (int32_t) IVALUE(cell);
            break;
          case GRN_CELL_FLOAT :
            v = (int32_t) FVALUE(cell);
            break;
          case GRN_CELL_TIME :
            v = (int32_t) cell->u.tv.tv_nsec / GRN_TIME_NSEC_PER_SEC * GRN_TIME_USEC_PER_SEC;
            break;
          }
          if (!obj) { if (!(obj = grn_obj_open(ctx, GRN_BULK, 0, 0))) { return NULL; }}
          grn_bulk_write(ctx, obj, (const char *)(&v), sizeof(int32_t));
        }
        break;
      case GRN_DB_UINT32 :
        {
          uint32_t v;
          switch (cell->header.type) {
          case GRN_CELL_STR :
            {
              int64_t iv = grn_atoll(STRVALUE(cell), STRVALUE(cell) + STRSIZE(cell), NULL);
              v = (uint32_t) iv;
            }
            break;
          case GRN_CELL_INT :
            v = (uint32_t) IVALUE(cell);
            break;
          case GRN_CELL_FLOAT :
            v = (uint32_t) FVALUE(cell);
            break;
          case GRN_CELL_TIME :
            v = (uint32_t) cell->u.tv.tv_nsec / GRN_TIME_NSEC_PER_SEC * GRN_TIME_USEC_PER_SEC;
            break;
          }
          if (!obj) { if (!(obj = grn_obj_open(ctx, GRN_BULK, 0, 0))) { return NULL; }}
          grn_bulk_write(ctx, obj, (const char *)(&v), sizeof(uint32_t));
        }
        break;
      case GRN_DB_INT64 :
        {
          int64_t v;
          switch (cell->header.type) {
          case GRN_CELL_STR :
            v = grn_atoll(STRVALUE(cell), STRVALUE(cell) + STRSIZE(cell), NULL);
            break;
          case GRN_CELL_INT :
            v = IVALUE(cell);
            break;
          case GRN_CELL_FLOAT :
            v = (int64_t) FVALUE(cell);
            break;
          case GRN_CELL_TIME :
            v = (int32_t) cell->u.tv.tv_nsec / GRN_TIME_NSEC_PER_SEC * GRN_TIME_USEC_PER_SEC;
            break;
          }
          if (!obj) { if (!(obj = grn_obj_open(ctx, GRN_BULK, 0, 0))) { return NULL; }}
          grn_bulk_write(ctx, obj, (const char *)(&v), sizeof(int64_t));
        }
        break;
      case GRN_DB_FLOAT :
        {
          double v;
          switch (cell->header.type) {
          case GRN_CELL_STR :
            { /* todo : support #i notation */
              char *str = STRVALUE(cell);
              int len = STRSIZE(cell);
              STR2DBL(str, len, v);
            }
            break;
          case GRN_CELL_INT :
            v = (double) IVALUE(cell);
            break;
          case GRN_CELL_FLOAT :
            v = FVALUE(cell);
            break;
          case GRN_CELL_TIME :
            v = ((double) cell->u.tv.tv_nsec) / GRN_TIME_NSEC_PER_SEC + cell->u.tv.tv_sec;
            break;
          }
          if (!obj) { if (!(obj = grn_obj_open(ctx, GRN_BULK, 0, 0))) { return NULL; }}
          grn_bulk_write(ctx, obj, (const char *)(&v), sizeof(double));
        }
        break;
      case GRN_DB_TIME :
        {
          grn_timeval v;
          switch (cell->header.type) {
          case GRN_CELL_STR :
            {
              int len = STRSIZE(cell);
              char *str = STRVALUE(cell);
              if (grn_str2timeval(str, len, &v)) {
                if (len > 3 && *str == '#' && str[1] == ':' && str[2] == '<') {
                  const char *cur;
                  v.tv_sec = grn_atoi(str + 3, str + len, &cur);
                  if (cur >= str + len || *cur != '.') {
                    QLWARN("illegal time format '%s'", str);
                  }
                  v.tv_nsec = grn_atoi(cur + 1, str + len, &cur);
                  if (cur >= str + len || *cur != '>') {
                    QLWARN("illegal time format '%s'", str);
                  }
                } else {
                  double dval = 0.0;
                  char *str = STRVALUE(cell);
                  int len = cell->u.b.size;
                  STR2DBL(str, len, dval);
                  v.tv_sec = (int32_t) dval;
                  v.tv_nsec = (int32_t) ((dval - v.tv_sec) * GRN_TIME_NSEC_PER_SEC);
                }
              }
            }
            break;
          case GRN_CELL_INT :
            v.tv_sec = (int32_t) IVALUE(cell);
            v.tv_nsec = 0;
            break;
          case GRN_CELL_FLOAT :
            v.tv_sec = (int32_t) FVALUE(cell);
            v.tv_nsec = (int32_t) ((FVALUE(cell) - v.tv_sec) * GRN_TIME_NSEC_PER_SEC);
            break;
          case GRN_CELL_TIME :
            v.tv_sec = cell->u.tv.tv_sec;
            v.tv_nsec = cell->u.tv.tv_nsec;
            break;
          }
          if (!obj) { if (!(obj = grn_obj_open(ctx, GRN_BULK, 0, 0))) { return NULL; }}
          grn_bulk_write(ctx, obj, (const char *)(&v), sizeof(grn_timeval));
        }
        break;
      default :
        if (BULKP(cell)) {
          if (!obj) { if (!(obj = grn_obj_open(ctx, GRN_BULK, 0, 0))) { return NULL; }}
          grn_bulk_write(ctx, obj, STRVALUE(cell), STRSIZE(cell));
        } else {
          if (obj) { GRN_BULK_REWIND(obj); }
        }
        break;
      }
    } else {
      /* todo : grn_type_any support
      grn_bulk_write(ctx, bulk, (const char *)(&cell->header.domain), sizeof(grn_id));
      */
      /* todo : nested table support.
       */
      grn_id id;
      switch (cell->header.type) {
      case GRN_CELL_OBJECT :
        if (cell->header.domain != rid) { return NULL; }
        id = cell->u.o.id;
        break;
      case GRN_CELL_STR :
        id = grn_table_add(ctx, range, STRVALUE(cell), STRSIZE(cell), NULL);
        break;
      default :
        if (VOIDP(cell)) { id = GRN_ID_NIL; }
        break;
      }
      if (!obj) { if (!(obj = grn_obj_open(ctx, GRN_BULK, 0, 0))) { return NULL; }}
      grn_bulk_write(ctx, obj, (const char *)&id, sizeof(grn_id));
    }
    obj->header.domain = rid;
  } else {
    switch (cell->header.type) {
    case GRN_CELL_STR :
      if (BULKP(cell)) {
        if (!obj) { if (!(obj = grn_obj_open(ctx, GRN_BULK, 0, 0))) { return NULL; }}
        grn_bulk_write(ctx, obj, STRVALUE(cell), STRSIZE(cell));
      } else {
        if (obj) { GRN_BULK_REWIND(obj); }
      }
      /* todo */
      break;
    case GRN_CELL_INT :
      /* todo */
      break;
    case GRN_CELL_FLOAT :
      /* todo */
      break;
    case GRN_CELL_TIME :
      /* todo */
      break;
    case GRN_CELL_LIST :
      obj = grn_obj_open(ctx, GRN_VECTOR, 0, 0);
      list2vector(ctx, cell, obj);
      break;
    case GRN_VECTOR :
      obj = (grn_obj *)cell->u.p.value;
      break;
    }
  }
  return obj;
}

inline static grn_cell *
column_value(grn_ctx *ctx, grn_obj *column, grn_id obj,
             grn_cell *args, grn_cell *res)
{
  grn_obj *value;
  if (!column) { return F; }
  if (VOIDP(args) || (PAIRP(args) && VOIDP(CAR(args)))) {
    if ((value = grn_obj_get_value(ctx, column, obj, NULL))) {
      switch (column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) {
      case GRN_OBJ_COLUMN_VECTOR :
        {
          grn_cell **rp = &res;
          grn_id *v = (grn_id *) GRN_BULK_HEAD(value);
          uint32_t s = GRN_BULK_VSIZE(value) / sizeof(grn_id);
          *rp = NIL;
          while (s--) {
            grn_cell *cell = grn_cell_new(ctx);
            if (cell) {
              obj_obj_bind(cell, column->header.domain, *v);
              if ((*rp = CONS(cell, NIL))) {
                rp = &CDR(*rp);
              }
            }
          }
        }
        break;
      default :
        res = obj2cell(ctx, value, res);
        break;
      }
      grn_obj_close(ctx, value);
    } else {
      res = F;
    }
  } else {
    grn_cell *car;
    int flags = GRN_OBJ_SET;
    POP(car, args);
    if ((value = cell2obj(ctx, car, column, NULL))) {
      res = grn_obj_set_value(ctx, column, obj, value, flags) ? F : car;
      grn_obj_close(ctx, value);
    } else {
      res = F;
    }
  }
  return res;
}

static grn_cell *
register_cell(grn_ctx *ctx, grn_obj *db_obj, const char *name, unsigned int name_size)
{
  if (name && name_size) {
    return INTERN2(name, name_size);
    // grn_ql_obj_bind(db_obj, res);
  } else {
    grn_cell *r = get_cell(ctx, db_obj);
    if (r != F) {
      r->header.impl_flags = 0;
      r->header.flags = 0;
      grn_ql_obj_bind(db_obj, r);
    }
    return r;
  }
}

static grn_cell *
table_create(grn_ctx *ctx, const char *name, unsigned int name_size,
             grn_obj_flags flags, grn_obj *domain,
             grn_obj *value_type, grn_id tokenizer)
{
  grn_obj *table = grn_table_create(ctx, name, name_size,
                                    NULL, flags, domain, value_type);
  if (!table) { QLERR("table create failed"); }
  grn_obj_set_info(ctx, table, GRN_INFO_DEFAULT_TOKENIZER, grn_ctx_at(ctx, tokenizer));
  return register_cell(ctx, table, name, name_size);
}

static grn_cell *
rec_obj_new(grn_ctx *ctx, grn_obj *domain, grn_obj *value_type)
{
  return table_create(ctx, NULL, 0, GRN_OBJ_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                      domain, value_type, GRN_DB_DELIMIT);
}

typedef struct {
  column_exp *ce;
  grn_cell *func;
  //  grn_cell *exprs;
  grn_cell *args;
  grn_operator op;
  //  grn_cell *objs;
  int32_t offset;
  int32_t limit;
  int mode;
  char *from;
  unsigned int fromsize;
  char *to;
  unsigned int tosize;
  column_exp *score_ce;
  grn_cell *score_func;
} match_spec;

inline static grn_cell *
match_prepare(grn_ctx *ctx, match_spec *spec, grn_id base, grn_cell *args)
{
  grn_cell *car, *expr;
  grn_obj *r = grn_ctx_at(ctx, base);
  POP(expr, args);
  spec->ce = column_exp_open(ctx, r, expr, NIL);
  if (EVAL_BY_FUNCALLP(spec->ce)) {
    spec->func = CAR(expr);
  } else {
    spec->func = NULL;
  }
  spec->offset = 0;
  spec->limit = 0;
  spec->mode = 0;
  spec->from = NULL;
  spec->fromsize = 0;
  spec->to = NULL;
  spec->tosize = 0;
  spec->op = GRN_OP_OR;
  spec->score_ce = NULL;
  spec->score_func = NULL;
  POP(car, args);
  if (car != NIL) {
    spec->score_ce = column_exp_open(ctx, r, car, NIL);
    if (EVAL_BY_FUNCALLP(spec->score_ce)) { spec->score_func = CAR(car); }
  }
  POP(expr, args);
  if (RECORDSP(expr)) {
    char ops[STRBUF_SIZE];
    uint16_t ops_size;
    if (expr->header.domain != base) { QLERR("table unmatch"); }
    POP(car, args);
    spec->op = GRN_OP_AND;
    if (!obj2str(car, ops, &ops_size)) {
      switch (*ops) {
      case '+': spec->op = GRN_OP_OR; break;
      case '-': spec->op = GRN_OP_BUT; break;
      case '*': spec->op = GRN_OP_AND; break;
      case '>': spec->op = GRN_OP_ADJUST; break;
      }
    }
  } else {
    char str[STRBUF_SIZE];
    uint16_t str_size;
    grn_obj *table = grn_ctx_at(ctx, base);
    if (INTP(expr)) { spec->offset = IVALUE(expr); }
    POP(expr, args);
    if (INTP(expr)) { spec->limit = IVALUE(expr); }
    if (spec->limit <= 0) { spec->limit += grn_table_size(ctx, table); }
    POP(expr, args);
    if (!obj2str(expr, str, &str_size)) {
      uint16_t i;
      for (i = 0; i < str_size; i++) {
        switch (str[i]) {
        case 'd' : spec->mode |= GRN_CURSOR_DESCENDING; break;
        case 'g' : spec->mode |= GRN_CURSOR_GT; break;
        case 'l' : spec->mode |= GRN_CURSOR_LT; break;
        }
      }
    }
    POP(expr, args);
    if (BULKP(expr)) {
      spec->from = STRVALUE(expr);
      spec->fromsize = STRSIZE(expr);
    }
    POP(expr, args);
    if (BULKP(expr)) {
      spec->to = STRVALUE(expr);
      spec->tosize = STRSIZE(expr);
    }
    expr = rec_obj_new(ctx, table, NULL);
    if (ERRP(ctx, GRN_WARN)) { return F; }
    spec->op = GRN_OP_OR;
  }
  //  spec->objs = CONS(expr, spec->exprs);
  return expr;
}

grn_obj *
grn_ql_obj_key(grn_ctx *ctx, grn_cell *obj, grn_obj *value)
{
  grn_obj *table = grn_ctx_at(ctx, obj->header.domain);
  grn_obj *accessor = grn_obj_column(ctx, table, ":key", 4);
  if (accessor) {
    value = grn_obj_get_value(ctx, accessor, obj->u.o.id, value);
    grn_obj_unlink(ctx, accessor);
  }
  return value;
}

inline static grn_cell *
obj2oid(grn_ctx *ctx, grn_cell *obj, grn_cell *res)
{
  grn_obj buf;
  if (obj->header.type != GRN_CELL_OBJECT) { return F; }
  GRN_TEXT_INIT(&buf, 0);
  grn_obj_inspect(ctx, obj, &buf, GRN_OBJ_INSPECT_ESC);
  if (res) {
    uint32_t size = GRN_BULK_VSIZE(&buf);
    char *value = GRN_MALLOC(size + 1);
    if (!value) {
      res = F;
      goto exit;
    }
    grn_cell_clear(ctx, res);
    res->header.impl_flags = GRN_OBJ_ALLOCATED;
    res->header.type = GRN_CELL_STR;
    res->u.b.size = size;
    res->u.b.value = value;
    memcpy(res->u.b.value, GRN_TEXT_VALUE(&buf), res->u.b.size + 1);
  } else {
    if (!(res = grn_ql_mk_string(ctx, GRN_TEXT_VALUE(&buf), GRN_BULK_VSIZE(&buf)))) {
      res = F;
    }
  }
exit :
  GRN_OBJ_FIN(ctx, &buf);
  return res;
}

inline static int
match_exec(grn_ctx *ctx, match_spec *spec, grn_id base, grn_id id)
{
  grn_cell *res;
  column_exp_exec(ctx, spec->ce, id);
  if (spec->func) {
    grn_cell *code = ctx->impl->code;
    ctx->impl->code = spec->func;
    res = spec->func->u.o.func(ctx, CDR(spec->ce->expr), &ctx->impl->co);
    ctx->impl->code = code;
  } else {
    res = grn_ql_eval(ctx, spec->ce->expr, NIL);
  }
  return res != F;
}

inline static int
score_exec(grn_ctx *ctx, match_spec *spec, grn_id base, grn_id id)
{
  grn_cell *res;
  column_exp_exec(ctx, spec->score_ce, id);
  if (spec->score_func) {
    grn_cell *code = ctx->impl->code;
    ctx->impl->code = spec->score_func;
    res = spec->score_func->u.o.func(ctx, CDR(spec->score_ce->expr), &ctx->impl->co);
    ctx->impl->code = code;
  } else {
    res = grn_ql_eval(ctx, spec->score_ce->expr, NIL);
  }
  switch (res->header.type) {
  case GRN_CELL_INT :
    return IVALUE(res);
  case GRN_CELL_FLOAT :
    return (int)FVALUE(res);
  default :
    return 0;
  }
}

static grn_rc
match_close(grn_ctx *ctx, match_spec *spec)
{
  if (spec->score_ce) { column_exp_close(ctx, spec->score_ce); }
  return column_exp_close(ctx, spec->ce);
}

// todo : refine
#define MAXCOLUMNS 0x100

struct _ins_stat {
  grn_cell *columns;
  int ncolumns;
  int nrecs;
};

typedef struct {
  grn_encoding encoding;
  char *cur;
  char *str_end;
} jctx;

static grn_cell *json_read(grn_ctx *ctx, jctx *jc, int keyp);

static grn_cell *
ha_table(grn_ctx *ctx, grn_cell *args, grn_ql_co *co)
{
  grn_id base;
  int load = 0;
  grn_obj *table;
  grn_cell *car, *res;
  char msg[STRBUF_SIZE];
  uint16_t msg_size;
  if (!(res = ctx->impl->code)) { QLERR("invalid receiver"); }
  base = ctx->impl->code->u.o.id;
  if (!(table = grn_ctx_at(ctx, base))) { QLERR("invalid table"); }
  GRN_QL_CO_BEGIN(co);
  POP(car, args);
  if (obj2str(car, msg, &msg_size)) { QLERR("invalid argument"); }
  switch (*msg) {
  case '\0' : /* get instance by key */
    {
      POP(car, args);
      if (obj2str(car, msg, &msg_size)) { QLERR("invalid argument"); }
      res = grn_ql_table_get(ctx, table, msg, msg_size, NULL);
    }
    break;
  case ':' :
    switch (msg[1]) {
    case 'c' :
    case 'C' :
      switch (msg[2]) {
      case 'l' : /* :clearlock */
      case 'L' :
        {
          res = grn_obj_is_locked(ctx, table) ? T : F;
          grn_obj_clear_lock(ctx, table);
        }
        break;
      case 'o' : /* :common-prefix-search */
      case 'O' :
        switch (msg[3]) {
        case 'm' :
        case 'M' :
          {
            grn_id id;
            POP(car, args);
            if (!BULKP(car)) { QLERR("invalid argument"); }
            id = grn_table_lcp_search(ctx, table, STRVALUE(car), STRSIZE(car));
            res = id ? grn_ql_obj_new(ctx, base, id) : F;
          }
          break;
        case 'l' : /* :columns */
        case 'L' :
          POP(car, args);
          if (RECORDSP(car)) {
            msg_size = 0;
            res = car;
          } else {
            if (obj2str(car, msg, &msg_size)) { QLERR("invalid argument"); }
            POP(res, args);
            if (res == NIL) {
              if (!(res = rec_obj_new(ctx, NULL, NULL))) { QLERR("rec_obj_new failed"); }
            } else if (!RECORDSP(res)) {
              QLERR("records object expected");
            }
          }
          grn_table_columns(ctx, table, msg, msg_size, get_obj(ctx, res));
          break;
        default :
          res = F;
          break;
        }
        break;
      }
      break;
    case 'd' :
    case 'D' :
      switch (msg[2]) {
      case 'e' :
      case 'E' :
        switch (msg[3]) {
        case 'f' : /* :def */
        case 'F' :
          {
            uint16_t name_size;
            int nsources = 0;
            grn_id sources[MAXCOLUMNS];
            grn_obj *column, *type;
            char name[STRBUF_SIZE];
            grn_obj_flags flags = GRN_OBJ_PERSISTENT; /* default */
            POP(car, args);
            if (obj2str(car, name, &name_size)) { QLERR("invalid argument"); }
            if (grn_obj_column(ctx, table, name, name_size)) { return T; }
            POP(car, args);
            type = get_obj(ctx, car);
            while (PAIRP(args)) {
              POP(car, args);
              if (PAIRP(car)) {
                grn_cell *col;
                while (PAIRP(car) && nsources < MAXCOLUMNS) {
                  POP(col, car);
                  if (!obj2str(col, msg, &msg_size)) {
                    grn_obj *source = grn_obj_column(ctx, type, msg, msg_size);
                    if (source) { sources[nsources++] = DB_OBJ(source)->id; }
                  }
                }
              } else {
                if (obj2str(car, msg, &msg_size)) { QLERR("invalid argument"); }
                switch (*msg) {
                case 'b' :
                case 'B' :
                  flags |= GRN_OBJ_RING_BUFFER;
                  break;
                case 'i' :
                case 'I' :
                  flags |= GRN_OBJ_COLUMN_INDEX;
                  break;
                case 'l' :
                case 'L' :
                  flags |= GRN_OBJ_COMPRESS_LZO;
                  break;
                case 'p' :
                case 'P' :
                  switch (msg[1]) {
                  case 'e' :
                  case 'E' :
                    flags |= GRN_OBJ_PERSISTENT;
                    break;
                  case 'o' :
                  case 'O' :
                    flags |= GRN_OBJ_WITH_POSITION;
                    break;
                  }
                  break;
                case 's' :
                case 'S' :
                  flags |= GRN_OBJ_WITH_SECTION;
                  break;
                case 't' :
                case 'T' :
                  flags &= ~GRN_OBJ_PERSISTENT;
                  break;
                case 'v' :
                case 'V' :
                  flags |= GRN_OBJ_COLUMN_VECTOR;
                  break;
                case 'w' :
                case 'W' :
                  flags |= GRN_OBJ_WITH_WEIGHT;
                  break;
                case 'z' :
                case 'Z' :
                  flags |= GRN_OBJ_COMPRESS_ZLIB;
                  break;
                }
              }
            }
            column = grn_column_create(ctx, table, name, name_size, NULL, flags, type);
            if (column) {
              if (nsources) {
                grn_obj source;
                GRN_TEXT_INIT(&source, GRN_OBJ_DO_SHALLOW_COPY);
                GRN_TEXT_SET_REF(&source, sources, nsources * sizeof(grn_id));
                grn_obj_set_info(ctx, column, GRN_INFO_SOURCE, &source);
              }
              msg_size = grn_obj_name(ctx, column, msg, GRN_PAT_MAX_KEY_SIZE);
              res = INTERN2(msg, msg_size);
            } else {
              res = F;
            }
          }
          break;
        case 'l' : /* :delete */
        case 'L' :
          POP(car, args);
          if (obj2str(car, msg, &msg_size)) { QLERR("invalid argument"); }
          res = grn_table_delete(ctx, table, msg, msg_size) ? T : F;
          break;
        default :
          res = F;
          break;
        }
        break;
      case 'i' : /* :difference */
      case 'I' :
        {
          if (PAIRP(args)) {
            POP(car, args);
            if (TABLEP(car)) {
              grn_obj *table2 = RVALUE(car);
              res = grn_table_difference(ctx, table, table2, table, table2) ? T : F;
            }
          }
        }
        break;
      default :
        res = F;
        break;
      }
      break;
    case 'e' : /* :extract */
    case 'E' :
      POP(car, args);
      if (!BULKP(car)) { QLERR("string expected"); }
      POP(res, args);
      if (res == NIL) {
        if (!(res = rec_obj_new(ctx, table, NULL))) { QLERR("rec_obj_new failed"); }
      } else if (!RECORDSP(res)) {
        QLERR("records object expected");
      }
      if (grn_table_search(ctx, table, STRVALUE(car), STRSIZE(car),
                           GRN_OP_TERM_EXTRACT, get_obj(ctx, res), GRN_OP_OR)
          < GRN_SUCCESS) {
        QLERR("term_extract failed");
      }
      break;
    case 'g' : /* :group */
    case 'G' :
      {
        column_exp *ce;
        POP(car, args);
        ce = column_exp_open(ctx, table, car, NIL);
        if (ce) {
          int n_results = 0;
          grn_table_group_result *rp, results[256];
          POP(car, args);
          for (rp = results; TABLEP(car); rp++) {
            memset(rp, 0, sizeof(grn_table_group_result));
            rp->table = get_obj(ctx, car);
            POP(car, args);
            if (INTP(car)) {
              rp->key_begin = IVALUE(car);
              POP(car, args);
              if (INTP(car)) {
                rp->key_end = IVALUE(car);
              } else {
                rp->key_end = rp->key_begin + 1;
              }
            } else {
              rp->key_begin = n_results;
              rp->key_end = rp->key_begin + 1;
            }
            if (++n_results >= 256) { break; }
          }
          if (!n_results) {
            for (rp = results; n_results < ce->n_keys; rp++, n_results++) {
              grn_id range = grn_obj_get_range(ctx, ce->keys[n_results].key);
              rp->table = get_obj(ctx, rec_obj_new(ctx, grn_ctx_at(ctx, range), NULL));
              rp->key_begin = n_results;
              rp->key_end = rp->key_begin + 1;
            }
          }
          grn_table_group(ctx, table, ce->keys, ce->n_keys, results, n_results);
          column_exp_close(ctx, ce);
          res = NIL;
          while (n_results--) {
            res = CONS(get_cell(ctx, results[n_results].table), res);
          }
        } else {
          QLERR("group key parse error");
        }
      }
      break;
    case 'i' : /* :intersect */
    case 'I' :
      {
        while (PAIRP(args)) {
          POP(car, args);
          if (!TABLEP(car)) { continue; }
          grn_table_setoperation(ctx, table, RVALUE(car), table, GRN_OP_AND);
        }
      }
      break;
    case 'j' : /* :jload */
    case 'J' :
      load = 1;
      break;
    case 'l' : /* :load */
    case 'L' :
      load = 2;
      break;
    case 'n' :
    case 'N' :
      {
        switch (msg[2]) {
        case 'e' : /* :new */
        case 'E' :
          {
            // todo : support array
            POP(car, args);
            if (obj2str(car, msg, &msg_size)) { return F; }
            res = grn_ql_table_add(ctx, table, msg, msg_size, NULL);
            if (res != F) {
              grn_cell cons, dummy;
              grn_obj *column;
              cons.header.type = GRN_CELL_LIST;
              cons.header.impl_flags = 0;
              cons.u.l.cdr = NIL;
              while (PAIRP(args)) {
                POP(car, args);
                if (obj2str(car, msg, &msg_size)) { break; }
                POP(car, args);
                cons.u.l.car = car;
                column = grn_obj_column(ctx, table, msg, msg_size);
                column_value(ctx, column, res->u.o.id, &cons, &dummy);
              }
            }
          }
          break;
        case 'r' : /* :nrecs */
        case 'R' :
          GRN_CELL_NEW(ctx, res);
          SETINT(res, grn_table_size(ctx, table));
          break;
        default :
          /* ambiguous message. todo : return error */
          res = F;
        }
      }
      break;
    case 'p' : /* :prefix-search */
    case 'P' :
      POP(car, args);
      if (!BULKP(car)) { QLERR("string expected"); }
      POP(res, args);
      if (res == NIL) {
        if (!(res = rec_obj_new(ctx, table, NULL))) { QLERR("rec_obj_new failed"); }
      } else if (!RECORDSP(res)) {
        QLERR("records object expected");
      }
      if (grn_table_search(ctx, table, STRVALUE(car), STRSIZE(car),
                           GRN_OP_PREFIX, get_obj(ctx, res), GRN_OP_OR)
          < GRN_SUCCESS) {
        QLERR("prefix search failed");
      }
      break;
    case 's' :
    case 'S' :
      switch (msg[2]) {
      case 'c' :
      case 'C' :
        switch (msg[3]) {
        case 'a' : /* :scan-select */
        case 'A' :
          {
            grn_rset_recinfo *ri;
            grn_id *rid;
            match_spec spec;
            grn_obj *rec;
            res = match_prepare(ctx, &spec, base, args);
            if (ERRP(ctx, GRN_WARN)) { return F; }
            rec = RVALUE(res);
            switch (spec.op) {
            case GRN_OP_OR :
              {
                grn_id id;
                grn_table_cursor *c;
                grn_rset_posinfo *pi = (grn_rset_posinfo *) &id;
                int n = 0, o = spec.offset, l = spec.limit;
                if (l) {
                  if ((c = grn_table_cursor_open(ctx, table,
                                                 spec.from, spec.fromsize,
                                                 spec.to, spec.tosize, 0, -1,
                                                 spec.mode))) {
                    while ((id = grn_table_cursor_next(ctx, c))) {
                      if (match_exec(ctx, &spec, base, id)) {
                        if (n++ >= o) {
                          /* todo : use GRN_SET_INT_ADD if !n_entries */
                          grn_table_add_v(ctx, rec, pi, sizeof(grn_id), (void **)&ri, NULL);
                          if (spec.score_ce) {
                            int score = score_exec(ctx, &spec, base, id);
                            grn_table_add_subrec(rec, ri, score, NULL, 0);
                          }
                          if (!--l) { break; }
                        }
                      }
                    }
                    grn_table_cursor_close(ctx, c);
                  }
                }
              }
              break;
            case GRN_OP_AND :
              GRN_HASH_EACH(ctx, (grn_hash *)rec, id, &rid, NULL, &ri, {
                if (!match_exec(ctx, &spec, base, *rid)) {
                  grn_hash_delete_by_id(ctx, (grn_hash *)rec, id, NULL);
                }
              });
              break;
            case GRN_OP_BUT :
              GRN_HASH_EACH(ctx, (grn_hash *)rec, id, &rid, NULL, &ri, {
                if (match_exec(ctx, &spec, base, *rid)) {
                  grn_hash_delete_by_id(ctx, (grn_hash *)rec, id, NULL);
                }
              });
              break;
            case GRN_OP_ADJUST :
              /* todo : support it */
              break;
            default :
              break;
            }
            match_close(ctx, &spec);
          }
          break;
        case 'h' : /* :schema */
        case 'H' :
          {
            grn_obj_flags flags;
            grn_encoding encoding;
            grn_obj *tokenizer;
            res = NIL;
            if (!(msg_size = grn_obj_name(ctx, table, msg, STRBUF_SIZE))) {
              return F;
            }
            if (grn_table_get_info(ctx, table, &flags, &encoding, &tokenizer)) {
              return F;
            }
            if (flags & GRN_OBJ_KEY_NORMALIZE) { res = CONS(INTERN(":normalize"), res); }
            switch (flags & GRN_OBJ_TABLE_TYPE_MASK) {
            case GRN_OBJ_TABLE_HASH_KEY :
              res = CONS(INTERN(":hash"), res);
              break;
            case GRN_OBJ_TABLE_PAT_KEY :
              res = CONS(INTERN(":pat"), res);
              break;
            case GRN_OBJ_TABLE_NO_KEY :
              res = CONS(INTERN(":surrogate"), res);
              break;
            }
            {
              char encstr[32] = ":";
              strcpy(encstr + 1, grn_enctostr(encoding));
              res = CONS(INTERN(encstr), res);
            }
            res = CONS(INTERN("ptable"),
                       CONS(CONS(INTERN("quote"),
                                 CONS(INTERN2(msg, msg_size), NIL)), res));
          }
          break;
        default :
          res = F;
          break;
        }
        break;
      case 'o' : /* :sort */
      case 'O' :
        {
          int limit = 0;
          column_exp *ce;
          POP(car, args);
          ce = column_exp_open(ctx, table, car, NIL);
          POP(car, args);
          if (!grn_obj2int(ctx, car)) { limit = car->u.i.i; }
          if (limit <= 0) { limit += grn_table_size(ctx, table); }
          POP(res, args);
          if (!RECORDSP(res)) {
            res = table_create(ctx, NULL, 0, GRN_OBJ_TABLE_NO_KEY,
                               NULL, table, GRN_DB_DELIMIT);
          }
          if (ce) {
            grn_table_sort(ctx, table, 0, limit, get_obj(ctx, res), ce->keys, ce->n_keys);
            column_exp_close(ctx, ce);
          } else {
            grn_table_sort(ctx, table, 0, limit, get_obj(ctx, res), NULL, 0);
          }
        }
        break;
      case 'u' :
      case 'U' :
        switch (msg[3]) {
        case 'f' : /* :suffix-search */
        case 'F' :
          POP(car, args);
          if (!BULKP(car)) { QLERR("string expected"); }
          POP(res, args);
          if (res == NIL) {
            if (!(res = rec_obj_new(ctx, table, NULL))) { QLERR("rec_obj_new failed"); }
          } else if (!RECORDSP(res)) {
            QLERR("records object expected");
          }
          if (grn_table_search(ctx, table, STRVALUE(car), STRSIZE(car),
                               GRN_OP_SUFFIX, get_obj(ctx, res), GRN_OP_OR)
              < GRN_SUCCESS) {
            QLERR("suffix search failed");
          }
          break;
        case 'b' : /* :subtract */
        case 'B' :
          {
            while (PAIRP(args)) {
              POP(car, args);
              if (!TABLEP(car)) { continue; }
              grn_table_setoperation(ctx, table, RVALUE(car), table, GRN_OP_BUT);
            }
          }
          break;
        default :
          res = F;
          break;
        }
        break;
      default :
        res = F;
        break;
      }
      break;
    case 't' :
      {
        int n;
        grn_id id = GRN_ID_NIL;
        for (n = 0; (id = grn_table_next(ctx, table, id)); n++) ;
        res = (n == grn_table_size(ctx, table)) ? T : F;
      }
      break;
    case 'T' :
      {
        int n = 0;
        grn_id id;
        grn_table_cursor *c;
        if ((c = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1,
                                       GRN_CURSOR_DESCENDING))) {
          for (; (id = grn_table_cursor_next(ctx, c)); n++) ;
          grn_table_cursor_close(ctx, c);
        }
        res = (n == grn_table_size(ctx, table)) ? T : F;
      }
      break;
    case 'u' : /* :undef */
    case 'U' :
      switch (msg[2]) {
      case 'n' :
      case 'N' :
        switch (msg[3]) {
        case 'd' :
        case 'D' :
          {
            grn_obj *column;
            POP(car, args);
            if (obj2str(car, msg, &msg_size)) { return F; }
            if (!(column = grn_obj_column(ctx, table, msg, msg_size))) { return F; }
            if (!(msg_size = grn_obj_name(ctx, column, msg, STRBUF_SIZE))) { return F; }
            res = grn_table_delete(ctx, ctx->impl->db, msg, msg_size) ? F : T;
          }
          break;
        case 'i' : /* :union */
        case 'I' :
          {
            while (PAIRP(args)) {
              POP(car, args);
              if (!TABLEP(car)) { continue; }
              grn_table_setoperation(ctx, table, RVALUE(car), table, GRN_OP_OR);
            }
          }
          break;
        default :
          res = F;
          break;
        }
        break;
      default :
        res = F;
        break;
      }
      break;
    case '+' : /* :+ (iterator next) */
      {
        grn_id id;
        POP(res, args);
        if (res->header.type == GRN_CELL_OBJECT &&
            res->header.domain == DB_OBJ(table)->id &&
            (id = grn_table_next(ctx, table, res->u.o.id))) {
          res->u.o.id = id;
        } else {
          res = F;
        }
      }
      break;
    case '\0' : /* : (iterator begin) */
      {
        grn_id id;
        id = grn_table_next(ctx, table, GRN_ID_NIL);
        if (id == GRN_ID_NIL) {
          res = F;
        } else {
          GRN_CELL_NEW(ctx, res);
          obj_obj_bind(res, DB_OBJ(table)->id, id);
        }
      }
      break;
    }
    break;
  default : /* :columnname */
    res = table_column(ctx, base, msg, msg_size);
    break;
  }
  if (load == 1) {
    int i;
    grn_cell *s, *saved;
    struct _ins_stat *stat;
    for (s = args, i = 0; PAIRP(s); s = CDR(s), i++) {
      car = CAR(s);
      if (obj2str(car, msg, &msg_size)) { QLERR("invalid argument"); }
      CAR(s) = table_column(ctx, base, msg, msg_size);
      if (CAR(s) == F) { QLERR("invalid argument"); }
    }
    if (!(saved = grn_cell_alloc(ctx, sizeof(struct _ins_stat)))) { return F; }
    stat = (struct _ins_stat *)saved->u.b.value;
    stat->columns = args;
    stat->ncolumns = i;
    stat->nrecs = 0;
    do {
      GRN_QL_CO_WAIT(co, saved);
      stat = (struct _ins_stat *)saved->u.b.value;
      if (BULKP(args) && STRSIZE(args)) {
        grn_cell *r, *o, obj, cons, dummy;
        jctx jc;
        jc.encoding = ctx->encoding;
        jc.cur = args->u.b.value;
        jc.str_end = args->u.b.value + args->u.b.size;
        r = json_read(ctx, &jc, 0);
        POP(car, r);
        if (car == INTERN("@")) { /* hash */
          for (s = r; CAR(s) != INTERN("::key"); s = CDR(s)) {
            if (!PAIRP(s)) { QLERR("invalid argument"); }
          }
          s = CADR(s);
          o = grn_ql_table_add(ctx, table, s->u.b.value, s->u.b.size, &obj);
          if (o != F) {
            cons.header.type = GRN_CELL_LIST;
            cons.header.impl_flags = 0;
            cons.u.l.cdr = NIL;
            while (PAIRP(r)) {
              grn_obj *column;
              POP(car, r);
              if (obj2str(car, msg, &msg_size)) { QLERR("invalid argument"); }
              POP(car, r);
              cons.u.l.car = car;
              column = get_column(ctx, base, msg, msg_size, NULL);
              column_value(ctx, column, obj.u.o.id, &cons, &dummy);
              grn_obj_unlink(ctx, column);
            }
            stat->nrecs++;
          }
        } else { /* array */
          if (!BULKP(car)) { QLERR("invalid argument"); }
          o = grn_ql_table_add(ctx, table, car->u.b.value, car->u.b.size, &obj);

          if (o != F) {
            cons.header.type = GRN_CELL_LIST;
            cons.header.impl_flags = 0;
            cons.u.l.cdr = NIL;
            for (s = stat->columns, i = 0; i < stat->ncolumns; s = CDR(s), i++) {
              grn_obj *column;
              POP(car, r);
              cons.u.l.car = car;
              column = grn_ctx_at(ctx, CAR(s)->u.o.id);
              column_value(ctx, column, obj.u.o.id, &cons, &dummy);
            }
            stat->nrecs++;
          }
        }
      } else {
        co->mode |= GRN_CTX_TAIL;
      }
      {
        grn_cell *_value = ctx->impl->value;
        ctx->impl->value = saved;
        grn_ctx_mgc(ctx);
        ctx->impl->value = _value;
      }
    } while (!(co->mode & (GRN_CTX_HEAD|GRN_CTX_TAIL)));
    if ((res = grn_cell_new(ctx))) {
      res->header.type = GRN_CELL_INT;
      res->u.i.i = stat->nrecs;
    } else {
      res = F;
    }
  } else if (load == 2) {
    int i;
    grn_cell *s;
    struct _ins_stat *stat;
    for (s = args, i = 0; PAIRP(s); s = CDR(s), i++) {
      car = CAR(s);
      if (obj2str(car, msg, &msg_size)) { QLERR("invalid argument"); }
      CAR(s) = table_column(ctx, base, msg, msg_size);
      if (CAR(s) == F) { QLERR("invalid argument"); }
    }
    if (!(s = grn_cell_alloc(ctx, sizeof(struct _ins_stat)))) { return F; }
    stat = (struct _ins_stat *)s->u.b.value; // todo : not GC safe
    stat->columns = args;
    stat->ncolumns = i + 1;
    stat->nrecs = 0;
    do {
      GRN_QL_CO_WAIT(co, stat);
      if (BULKP(args) && STRSIZE(args)) {
        const char *tokbuf[MAXCOLUMNS];
        grn_obj *column;
        grn_cell val, obj, cons, dummy;
        cons.header.type = GRN_CELL_LIST;
        cons.header.impl_flags = 0;
        cons.u.l.car = &val;
        cons.u.l.cdr = NIL;
        val.header.type = GRN_CELL_STR;
        if (grn_str_tok(args->u.b.value, STRSIZE(args), '\t', tokbuf, MAXCOLUMNS, NULL) == stat->ncolumns) {
          grn_cell *o;
          if (grn_obj_lock(ctx, ctx->impl->db, GRN_ID_NIL, -1)) {
            GRN_LOG(ctx, GRN_LOG_CRIT, "ha_table::load lock failed");
          } else {
            o = grn_ql_table_add(ctx, table, args->u.b.value,
                                tokbuf[0] - args->u.b.value, &obj);
            if (o != F) {
              for (s = stat->columns, i = 1; i < stat->ncolumns; s = CDR(s), i++) {
                val.u.b.value = (char *)tokbuf[i - 1] + 1;
                val.u.b.size = tokbuf[i] - val.u.b.value;
                unesc(ctx, &val);
                column = grn_ctx_at(ctx, CAR(s)->u.o.id);
                column_value(ctx, column, obj.u.o.id, &cons, &dummy);
              }
              stat->nrecs++;
            }
            grn_obj_unlock(ctx, ctx->impl->db, GRN_ID_NIL);
          }
        }
      } else {
        co->mode |= GRN_CTX_TAIL;
      }
    } while (!(co->mode & (GRN_CTX_HEAD|GRN_CTX_TAIL)));
    if ((res = grn_cell_new(ctx))) {
      res->header.type = GRN_CELL_INT;
      res->u.i.i = stat->nrecs;
    } else {
      res = F;
    }
  }
  GRN_QL_CO_END(co);
  return res;
}

static grn_cell *
ha_object(grn_ctx *ctx, grn_cell *args, grn_ql_co *co)
{
  char msg[STRBUF_SIZE];
  uint16_t msg_size;
  grn_cell *obj, *car, *res;
  if (!(obj = res = ctx->impl->code)) { QLERR("invalid receiver"); }
  while (PAIRP(args)) {
    POP(car, args);
    if (obj2str(car, msg, &msg_size)) { QLERR("invalid message"); }
    if (*msg == ':' && msg[1] == 'i') {
      res = obj2oid(ctx, obj, NULL);
    } else {
      grn_obj *domain = grn_ctx_at(ctx, obj->header.domain);
      grn_obj *column = grn_obj_column(ctx, domain, msg, msg_size);
      if (!column) {
        QLERR("invalid column %s", msg);
      }
      res = column_value(ctx, column, obj->u.o.id, args, NULL);
      grn_obj_unlink(ctx, column);
      POP(car, args);
    }
  }
  return res;
}

static grn_cell *
ha_query(grn_ctx *ctx, grn_cell *args, grn_ql_co *co)
{
  /* args: (str1@bulk) (str2@bulk) .. */
  grn_rc rc;
  grn_cell *x;
  const char **strs;
  grn_query *q;
  unsigned int *str_lens;
  int nstrs, found = 0, score = 0;
  if (!PAIRP(args) || !BULKP(CAR(args))) { QLERR("invalid argument"); }
  for (x = args, nstrs = 0; PAIRP(x) && BULKP(CAR(x)); x = CDR(x)) { nstrs++; }
  if (!(strs = GRN_MALLOC(sizeof(intptr_t) * nstrs * 2))) {
    QLERR("malloc failed");
  }
  str_lens = (unsigned int *)&strs[nstrs];
  for (x = args, nstrs = 0; PAIRP(x) && BULKP(CAR(x)); x = CDR(x)) {
    strs[nstrs] = STRVALUE(CAR(x));
    str_lens[nstrs] = STRSIZE(CAR(x));
    nstrs++;
  }
  q = (grn_query *)ctx->impl->code->u.p.value;
  rc = grn_query_scan(ctx, q, strs, str_lens, nstrs, GRN_QUERY_SCAN_NORMALIZE, &found, &score);
  GRN_FREE(strs);
  if (rc) { QLERR("grn_query_scan failed"); }
  if (!found) { return F; }
  GRN_CELL_NEW(ctx, x);
  SETINT(x, score);
  return x;
}

static grn_cell *
grn_obj_query(grn_ctx *ctx, const char *str, unsigned int str_len,
              grn_operator default_op, int max_exprs, grn_encoding encoding)
{
  grn_query *q;
  grn_cell *res = grn_cell_new(ctx);
  if (!res || !(q = grn_query_open(ctx, str, str_len, default_op, max_exprs))) {
    return NULL;
  }
  res->header.type = GRN_QUERY;
  res->header.impl_flags = GRN_CELL_NATIVE|GRN_OBJ_ALLOCATED;
  res->u.p.value = (grn_obj *)q;
  res->u.p.func = ha_query;
 return res;
}

static void
uvector2str(grn_ctx *ctx, grn_obj *obj, grn_obj *buf)
{
  grn_obj *range = grn_ctx_at(ctx, obj->header.domain);
  if (range && range->header.type == GRN_TYPE) {
    // todo
  } else {
    grn_id *v = (grn_id *)GRN_BULK_HEAD(obj), *ve = (grn_id *)GRN_BULK_CURR(obj);
    if (v < ve) {
      for (;;) {
        grn_table_get_key2(ctx, range, *v, buf);
        v++;
        if (v < ve) {
          GRN_TEXT_PUTC(ctx, buf, ' ');
        } else {
          break;
        }
      }
    }
  }
}

static grn_cell *
ha_column(grn_ctx *ctx, grn_cell *args, grn_ql_co *co)
{
  char msg[STRBUF_SIZE];
  uint16_t msg_size;
  grn_id base;
  grn_cell *car, *res;
  grn_obj *column;
  if (!(res = ctx->impl->code)) { QLERR("invalid receiver"); }
  base = ctx->impl->code->u.o.id;
  if (!(column = grn_ctx_at(ctx, base))) { QLERR("grn_ctx_get failed"); }
  POP(car, args);
  if (obj2str(car, msg, &msg_size)) { QLERR("invalid message"); }
  switch (*msg) {
  case '\0' :
    {
      if (IDX_COLUMNP(ctx->impl->code)) {
        grn_cell *q;
        grn_operator op;
        POP(q, args);
        if (!QUERYP(q)) {
          if (!BULKP(q)) { return F; }
          if (!(q = grn_obj_query(ctx, q->u.b.value, q->u.b.size, GRN_OP_AND, 32, ctx->encoding))) {
            QLERR("query_obj_new failed");
          }
        }
        /* TODO: specify record unit */
        /* (idxcolumn query ((column1 weight1) (column2 weight2) ...) records operator+ */
        POP(car, args);
        /* TODO: handle weights */
        POP(res, args);
        if (RECORDSP(res)) {
          char ops[STRBUF_SIZE];
          uint16_t ops_size;
          op = GRN_OP_AND;
          POP(car, args);
          if (!obj2str(car, ops, &ops_size)) {
            switch (*ops) {
            case '+': op = GRN_OP_OR; break;
            case '-': op = GRN_OP_BUT; break;
            case '*': op = GRN_OP_AND; break;
            case '>': op = GRN_OP_ADJUST; break;
            }
          }
        } else {
          grn_obj *table;
          if (!(table = grn_ctx_at(ctx, DB_OBJ(column)->range))) { return F; }
          res = rec_obj_new(ctx, table, NULL);
          if (ERRP(ctx, GRN_WARN)) { return F; }
          op = GRN_OP_OR;
        }
        grn_obj_search(ctx, column, (grn_obj *)q->u.p.value, RVALUE(res), op, NULL);
      } else {
        char name[STRBUF_SIZE];
        uint16_t name_size;
        grn_obj *table;
        POP(car, args);
        if (obj2str(car, name, &name_size)) { return F; }
        if (!(table = grn_column_table(ctx, column))) { return F; }
        res = grn_ql_table_get(ctx, table, name, name_size, NULL);
        if (res != F) {
          // grn_obj_lock(ctx, ctx->impl->db, GRN_ID_NIL, -1))
          column_value(ctx, column, res->u.o.id, args, res);
          // grn_obj_unlock(ctx, ctx->impl->db, GRN_ID_NIL);
        }
      }
    }
    break;
  case ':' :
    switch (msg[1]) {
    case 's' : /* :schema */
    case 'S' :
      {
        switch (column->header.type) {
        case GRN_COLUMN_FIX_SIZE  :
        case GRN_COLUMN_VAR_SIZE  :
          res = CONS(get_cell(ctx, grn_ctx_at(ctx, DB_OBJ(column)->range)), NIL);
          break;
        case GRN_COLUMN_INDEX :
          {
            /* todo :
            grn_db_trigger *t;
            res = CONS(INTERN("::match"), CONS(NIL, NIL));
            for (t = column->triggers; t; t = t->next) {
              if (t->type == grn_db_index_target) {
                res = CONS(get_cell(ctx, grn_ctx_at(ctx, t->target)), res);
              }
            }
            */
            res = CONS(INTERN(":index"), CONS(CONS(INTERN("quote"), CONS(res, NIL)), NIL));
          }
          break;
        case GRN_CELL_PSEUDO_COLUMN    :
          QLERR("not supported yet");
          break;
        default :
          QLERR("invalid column type");
          break;
        }
        /*
        {
          char *p, buf[GRN_PAT_MAX_KEY_SIZE];
          strcpy(buf, _grn_pat_key(ctx, ctx->impl->db->keys, base));
          if (!(p = strchr(buf, '.'))) { QLERR("invalid columnname %s", buf); }
          *p = ':';
          res = CONS(INTERN("::def"), CONS(INTERN(p), res));
          *p = '\0';
          res = CONS(INTERN(buf), res);
        }
        */
      }
      break;
    }
    break;
  }
  return res;
}

void
grn_ql_obj_bind(grn_obj *obj, grn_cell *symbol)
{
  symbol->header.type = obj->header.type;
  symbol->header.impl_flags |= GRN_CELL_NATIVE;
  symbol->u.o.id = DB_OBJ(obj)->id;
  symbol->header.domain = DB_OBJ(obj)->header.domain;
  switch (symbol->header.type) {
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_NO_KEY :
    symbol->u.o.func = ha_table;
    break;
  case GRN_COLUMN_FIX_SIZE :
    symbol->u.o.func = ha_column;
    break;
  case GRN_COLUMN_VAR_SIZE :
    symbol->u.o.func = ha_column;
    break;
  case GRN_COLUMN_INDEX :
    symbol->u.o.func = ha_column;
    break;
  default :
    symbol->u.o.func = ha_void;
    break;
  }
}

#define PVALUE(obj,type) ((type *)((obj)->u.p.value))

inline static void
snip_obj_bind(grn_cell *obj, grn_snip *snip)
{
  obj->header.type = GRN_SNIP;
  obj->header.impl_flags = GRN_CELL_NATIVE|GRN_OBJ_ALLOCATED;
  obj->u.p.value = (grn_obj *)snip;
  obj->u.p.func = ha_snip;
}

struct _patsnip_spec {
  grn_obj *table;
  int width;
  int max_results;
  column_exp *ce;
};

void
grn_obj_patsnip_spec_close(grn_ctx *ctx, patsnip_spec *spec)
{
  if (spec) {
    column_exp_close(ctx, spec->ce);
    GRN_FREE(spec);
  }
}

static grn_cell *
ha_snip(grn_ctx *ctx, grn_cell *args, grn_ql_co *co)
{
  /* args: (str@bulk) */
  if (!PAIRP(args) || !BULKP(CAR(args))) { QLERR("invalid argument"); }
  switch (ctx->impl->code->header.type) {
  case GRN_SNIP :
    {
      grn_obj buf;
      unsigned int i, len, max_len, nresults;
      grn_snip *s = PVALUE(ctx->impl->code, grn_snip);
      grn_cell *v, *str = CAR(args), *spc = PAIRP(CDR(args)) ? CADR(args) : NIL;
      if ((grn_snip_exec(ctx, s, str->u.b.value, str->u.b.size, &nresults, &max_len))) {
        QLERR("grn_snip_exec failed");
      }
      GRN_TEXT_INIT(&buf, 0);
      if (grn_bulk_resize(ctx, &buf, max_len)) { QLERR("grn_bulk_resize failed"); }

      if (nresults) {
        for (i = 0; i < nresults; i++) {
          if (i && spc != NIL) { grn_obj_inspect(ctx, spc, &buf, 0); }
          if (grn_bulk_reserve(ctx, &buf, max_len)) {
            grn_bulk_fin(ctx, &buf);
            QLERR("grn_bulk_space failed");
          }
          if ((grn_snip_get_result(ctx, s, i, buf.u.b.curr, &len))) {
            grn_bulk_fin(ctx, &buf);
            QLERR("grn_snip_get_result failed");
          }
          buf.u.b.curr += len;
        }
      } else {
        char *ss = str->u.b.value, *se = str->u.b.value + str->u.b.size;
        if (grn_substring(ctx, &ss, &se, 0, s->width, ctx->encoding)) {
          QLERR("grn_substring failed");
        }
        grn_bulk_write(ctx, &buf, ss, se - ss);
      }
      GRN_STR2OBJ(ctx, &buf, v);
      return v;
    }
    break;
  case GRN_PATSNIP :
    {
      grn_obj buf;
      patsnip_spec *spec = PVALUE(ctx->impl->code, patsnip_spec);
      off_t off = 0;
      const char *rest;
      grn_pat_scan_hit sh[1024];
      grn_cell *v, *expr, *str = CAR(args);
      char *string = STRVALUE(str);
      size_t len = STRSIZE(str);
      GRN_TEXT_INIT(&buf, 0);
      if (grn_bulk_resize(ctx, &buf, len)) { QLERR("grn_bulk_resize failed."); }
      while (off < len) {
        grn_obj *table = spec->table;
        int i, nhits = grn_pat_scan(ctx, (grn_pat *)table, string + off, len - off,
                                    sh, 1024, &rest);
        for (i = 0, off = 0; i < nhits; i++) {
          if (sh[i].offset < off) { continue; } /* skip overlapping region. */
          grn_bulk_write(ctx, &buf, string + off, sh[i].offset - off);
          column_exp_exec(ctx, spec->ce, sh[i].id);
          expr = spec->ce->expr;
          //          expr = grn_ql_eval(ctx, spec->ce->expr, NIL);
          POP(v, expr);
          grn_obj_inspect(ctx, grn_ql_eval(ctx, v, NIL), &buf, 0);
          grn_bulk_write(ctx, &buf, string + sh[i].offset, sh[i].length);
          POP(v, expr);
          grn_obj_inspect(ctx, grn_ql_eval(ctx, v, NIL), &buf, 0);
          off = sh[i].offset + sh[i].length;
        }
        if (string + off < rest) {
          grn_bulk_write(ctx, &buf, string + off, rest - (string + off));
        }
        off = rest - string;
      }
      GRN_STR2OBJ(ctx, &buf, v);
      return v;
    }
    break;
  default :
    QLERR("snip failed. invalid expr");
  }
}

static void disp_j(grn_ctx *ctx, grn_cell *obj, grn_obj *buf);

static void
disp_j_with_format(grn_ctx *ctx, grn_cell *args, grn_obj *buf)
{
  grn_cell *car;
  POP(car, args);
  switch (car->header.type) {
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_NO_KEY :
    {
      grn_id id, base;
      grn_cell *columns;
      int i, offset = 0, limit = 10, flags = GRN_CURSOR_ASCENDING;
      grn_obj *r = RVALUE(car);
      column_exp *ce;
      base = car->u.o.id;
      POP(columns, args);
      if (!PAIRP(columns)) {
        disp_j(ctx, car, buf);
        return;
      }
      if ((ce = column_exp_open(ctx, r, columns, NIL))) {
        POP(car, args);
        if (!grn_obj2int(ctx, car)) { offset = car->u.i.i; }
        POP(car, args);
        if (!grn_obj2int(ctx, car)) { limit = car->u.i.i; }
        if (limit <= 0) { limit += grn_table_size(ctx, r); }
        POP(car, args);
        {
          char msg[STRBUF_SIZE];
          uint16_t msg_size;
          if (!obj2str(car, msg, &msg_size) && (*msg == 'd')) {
            flags |= GRN_CURSOR_DESCENDING;
          }
        }
        {
          grn_table_cursor *tc = grn_table_cursor_open_by_id(ctx, r, offset,
                                                             offset + limit, flags);
          GRN_TEXT_PUTC(ctx, buf, '[');
          for (i = 0; (id = grn_table_cursor_next(ctx, tc)); i++) {
            if (i) { GRN_TEXT_PUTS(ctx, buf, ", "); }
            column_exp_exec(ctx, ce, id);
            disp_j(ctx, ce->expr, buf);
          }
          GRN_TEXT_PUTC(ctx, buf, ']');
          grn_table_cursor_close(ctx, tc);
        }
        column_exp_close(ctx, ce);
      }
    }
    break;
  case GRN_UVECTOR :
    {
      column_exp *ce;
      grn_cell *parameter, *columns;
      grn_obj *u = car->u.p.value, *r = grn_ctx_at(ctx, u->header.domain);
      POP(parameter, args);
      POP(columns, args);
      if (!PAIRP(columns)) {
        disp_j(ctx, car, buf);
        return;
      }
      if ((ce = column_exp_open(ctx, r, columns, parameter))) {
        /*
        POP(car, args);
        if (!grn_obj2int(ctx, car)) { offset = car->u.i.i; }
        POP(car, args);
        if (!grn_obj2int(ctx, car)) { limit = car->u.i.i; }
        */
        {
          int i;
          grn_id *v = (grn_id *)GRN_BULK_HEAD(u), *ve = (grn_id *)GRN_BULK_CURR(u);
          GRN_TEXT_PUTC(ctx, buf, '[');
          for (i = 0; v < ve; v++, i++) {
            if (i) { GRN_TEXT_PUTS(ctx, buf, ", "); }
            column_exp_exec(ctx, ce, *v);
            disp_j(ctx, ce->expr, buf);
          }
          GRN_TEXT_PUTC(ctx, buf, ']');
        }
        column_exp_close(ctx, ce);
      }
    }
    break;
  case GRN_CELL_OBJECT :
    {
      grn_cell *columns;
      column_exp *ce;
      grn_obj *r = get_domain(ctx, car);
      POP(columns, args);
      if (!PAIRP(columns)) {
        disp_j(ctx, car, buf);
        return;
      }
      if ((ce = column_exp_open(ctx, r, columns, NIL))) {
        column_exp_exec(ctx, ce, car->u.o.id);
        disp_j(ctx, ce->expr, buf);
        column_exp_close(ctx, ce);
      }
    }
    break;
  default :
    disp_j(ctx, car, buf);
    if (ERRP(ctx, GRN_WARN)) { return; }
    break;
  }
}

static void
disp_j(grn_ctx *ctx, grn_cell *obj, grn_obj *buf)
{
  if (!obj || obj == NIL) {
    GRN_TEXT_PUTS(ctx, buf, "[]");
  } else if (obj == T) {
    GRN_TEXT_PUTS(ctx, buf, "true");
  } else if (obj == F) {
    GRN_TEXT_PUTS(ctx, buf, "false");
  } else {
    switch (obj->header.type) {
    case GRN_VOID :
      if (SYMBOLP(obj) && obj != INTERN("null")) {
        uint16_t size = 0;
        const char *r = _grn_hash_strkey_by_val(obj, &size);
        if (size && (*r == ':')) {
          r++;
          size--;
        }
        grn_text_esc(ctx, buf, r, size);
      } else {
        GRN_TEXT_PUTS(ctx, buf, "null");
      }
      break;
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_NO_KEY :
      {
        int i;
        grn_id id;
        grn_cell o;
        grn_obj *r = RVALUE(obj);
        {
          grn_table_cursor *tc = grn_table_cursor_open_by_id(ctx, r, 0, 0,
                                                             GRN_CURSOR_ASCENDING);
          GRN_TEXT_PUTC(ctx, buf, '[');
          for (i = 0; (id = grn_table_cursor_next(ctx, tc)); i++) {
            if (i) { GRN_TEXT_PUTS(ctx, buf, ", "); }
            obj_obj_bind(&o, obj->u.o.id, id);
            disp_j(ctx, &o, buf);
            if (ERRP(ctx, GRN_WARN)) { return; }
          }
          GRN_TEXT_PUTC(ctx, buf, ']');
          grn_table_cursor_close(ctx, tc);
        }
      }
      break;
    case GRN_CELL_LIST :
      if (CAR(obj) == INTERN(":")) {
        disp_j_with_format(ctx, CDR(obj), buf);
        if (ERRP(ctx, GRN_WARN)) { return; }
      } else if (CAR(obj) == INTERN("@")) {
        int o;
        GRN_TEXT_PUTC(ctx, buf, '{');
        for (obj = CDR(obj), o = 0;; o = 1 - o) {
          if (PAIRP(obj)) {
            disp_j(ctx, CAR(obj), buf);
            if (ERRP(ctx, GRN_WARN)) { return; }
          }
          if ((obj = CDR(obj)) && (obj != NIL)) {
            if (PAIRP(obj)) {
              GRN_TEXT_PUTS(ctx, buf, o ? ", " : ": ");
            } else {
              GRN_TEXT_PUTS(ctx, buf, " . ");
              disp_j(ctx, obj, buf);
              if (ERRP(ctx, GRN_WARN)) { return; }
              GRN_TEXT_PUTC(ctx, buf, '}');
              break;
            }
          } else {
            GRN_TEXT_PUTC(ctx, buf, '}');
            break;
          }
        }
      } else {
        GRN_TEXT_PUTC(ctx, buf, '[');
        for (;;) {
          disp_j(ctx, CAR(obj), buf);
          if (ERRP(ctx, GRN_WARN)) { return; }
          if ((obj = CDR(obj)) && (obj != NIL)) {
            if (PAIRP(obj)) {
              GRN_TEXT_PUTS(ctx, buf, ", ");
            } else {
              GRN_TEXT_PUTS(ctx, buf, " . ");
              disp_j(ctx, obj, buf);
              if (ERRP(ctx, GRN_WARN)) { return; }
              GRN_TEXT_PUTC(ctx, buf, ']');
              break;
            }
          } else {
            GRN_TEXT_PUTC(ctx, buf, ']');
            break;
          }
        }
      }
      break;
    case GRN_CELL_OBJECT :
      {
        grn_obj key;
        GRN_TEXT_INIT(&key, 0);
        grn_ql_obj_key(ctx, obj, &key);
        grn_text_esc(ctx, buf, GRN_BULK_HEAD(&key), GRN_BULK_VSIZE(&key));
        grn_obj_close(ctx, &key);
      }
      break;
    case GRN_CELL_TIME :
      {
        double dv= obj->u.tv.tv_sec;
        dv += obj->u.tv.tv_nsec / GRN_TIME_NSEC_PER_SEC_F;
        grn_text_ftoa(ctx, buf, dv);
      }
      break;
    case GRN_UVECTOR :
      {
        grn_obj tmp;
        GRN_TEXT_INIT(&tmp, 0);
        uvector2str(ctx, obj->u.p.value, &tmp);
        grn_text_esc(ctx, buf, GRN_BULK_HEAD(&tmp), GRN_BULK_VSIZE(&tmp));
        grn_obj_close(ctx, &tmp);
      }
      break;
    default :
      grn_obj_inspect(ctx, obj, buf, GRN_OBJ_INSPECT_ESC|GRN_OBJ_INSPECT_SYMBOL_AS_STR);
      break;
    }
  }
}

static void disp_t(grn_ctx *ctx, grn_cell *obj, grn_obj *buf, int *f);

static void
disp_t_with_format(grn_ctx *ctx, grn_cell *args, grn_obj *buf, int *f)
{
  grn_cell *car;
  POP(car, args);
  switch (car->header.type) {
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_NO_KEY :
    {
      grn_id id, base;
      grn_cell *columns;
      int i, offset = 0, limit = 10, flags = GRN_CURSOR_ASCENDING;
      grn_obj *r = RVALUE(car);
      column_exp *ce;
      base = car->u.o.id;
      POP(columns, args);
      if (!PAIRP(columns)) {
        disp_t(ctx, car, buf, f);
        return;
      }
      if ((ce = column_exp_open(ctx, r, columns, NIL))) {
        POP(car, args);
        if (!grn_obj2int(ctx, car)) { offset = car->u.i.i; }
        POP(car, args);
        if (!grn_obj2int(ctx, car)) { limit = car->u.i.i; }
        if (limit <= 0) { limit += grn_table_size(ctx, r); }
        POP(car, args);
        {
          char msg[STRBUF_SIZE];
          uint16_t msg_size;
          if (!obj2str(car, msg, &msg_size) && (*msg == 'd')) {
            flags |= GRN_CURSOR_DESCENDING;
          }
        }
        {
          grn_table_cursor *tc = grn_table_cursor_open_by_id(ctx, r, offset,
                                                             offset + limit, flags);
          for (i = 0; (id = grn_table_cursor_next(ctx, tc)); i++) {
            if (*f) { ctx->impl->output(ctx, GRN_CTX_MORE, ctx->impl->data.ptr); *f = 0; }
            column_exp_exec(ctx, ce, id);
            disp_t(ctx, ce->expr, buf, f);
          }
          grn_table_cursor_close(ctx, tc);
        }
        column_exp_close(ctx, ce);
      }
    }
    break;
  case GRN_CELL_OBJECT :
    {
      grn_cell *columns;
      column_exp *ce;
      grn_obj *r = get_domain(ctx, car);
      POP(columns, args);
      if (!PAIRP(columns)) {
        disp_t(ctx, car, buf, f);
        return;
      }
      if ((ce = column_exp_open(ctx, r, columns, NIL))) {
        column_exp_exec(ctx, ce, car->u.o.id);
        disp_t(ctx, ce->expr, buf, f);
        column_exp_close(ctx, ce);
      }
    }
    break;
  default :
    disp_t(ctx, car, buf, f);
    break;
  }
}

inline static void
bulk_tsv_esc(grn_ctx *ctx, grn_obj *buf, const char *s, int len)
{
  const char *e;
  unsigned int l;
  for (e = s + len; s < e; s += l) {
    if (!(l = grn_charlen(ctx, s, e))) { break; }
    if (l == 1) {
      switch (*s) {
      case '\t' :
        grn_bulk_write(ctx, buf, "\\t", 2);
        break;
#ifdef GRN_QL_ESCAPE_NEWLINE
      case '\n' :
        grn_bulk_write(ctx, buf, "\\n", 2);
        break;
      case '\r' :
        grn_bulk_write(ctx, buf, "\\r", 2);
        break;
#endif /* GRN_QL_ESCAPE_NEWLINE */
      case '\\' :
        grn_bulk_write(ctx, buf, "\\\\", 2);
        break;
      default :
        GRN_TEXT_PUTC(ctx, buf, *s);
      }
    } else {
      grn_bulk_write(ctx, buf, s, l);
    }
  }
}

static void
disp_t(grn_ctx *ctx, grn_cell *obj, grn_obj *buf, int *f)
{
  if (!obj || obj == NIL) {
    GRN_TEXT_PUTS(ctx, buf, "()"); *f = 1;
  } else if (obj == T) {
    GRN_TEXT_PUTS(ctx, buf, "#t"); *f = 1;
  } else if (obj == F) {
    GRN_TEXT_PUTS(ctx, buf, "#f"); *f = 1;
  } else {
    switch (obj->header.type) {
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_NO_KEY :
      {
        int i;
        grn_id id;
        grn_cell o;
        grn_obj *r = RVALUE(obj);
        grn_table_cursor *tc = grn_table_cursor_open_by_id(ctx, r, 0, 0,
                                                           GRN_CURSOR_ASCENDING);
        for (i = 0; (id = grn_table_cursor_next(ctx, tc)); i++) {
          obj_obj_bind(&o, obj->u.o.id, id);
          if (*f) { ctx->impl->output(ctx, GRN_CTX_MORE, ctx->impl->data.ptr); *f = 0; }
          disp_t(ctx, &o, buf, f);
        }
        grn_table_cursor_close(ctx, tc);
      }
      break;
    case GRN_CELL_LIST :
      if (CAR(obj) == INTERN(":")) {
        disp_t_with_format(ctx, CDR(obj), buf, f);
      } else if (CAR(obj) == INTERN("@")) {
        int o0, o;
        grn_cell *val = CDR(obj);
        for (o0 = 0; o0 <= 1; o0++) {
          if (*f) { ctx->impl->output(ctx, GRN_CTX_MORE, ctx->impl->data.ptr); *f = 0; }
          for (obj = val, o = o0;; o = 1 - o) {
            if (!o) { disp_t(ctx, CAR(obj), buf, f); }
            if ((obj = CDR(obj)) && (obj != NIL)) {
              if (PAIRP(obj)) {
                if (!o && PAIRP(CDR(obj))) { GRN_TEXT_PUTC(ctx, buf, '\t'); *f = 1; }
              } else {
                if (!o) {
                  GRN_TEXT_PUTC(ctx, buf, '\t'); *f = 1; /* dot pair */
                  disp_t(ctx, obj, buf, f);
                }
                break;
              }
            } else {
              break;
            }
          }
        }
      } else {
        grn_cell *car;
        for (;;) {
          POP(car, obj);
          if (PAIRP(car)) {
            car = grn_ql_eval(ctx, car, NIL);
          }
          disp_t(ctx, car, buf, f);
          if ((obj != NIL)) {
            if (PAIRP(obj)) {
              GRN_TEXT_PUTC(ctx, buf, '\t'); *f = 1;
            } else {
              GRN_TEXT_PUTC(ctx, buf, '\t'); *f = 1; /* dot pair */
              disp_t(ctx, obj, buf, f);
              break;
            }
          } else {
            break;
          }
        }
      }
      break;
    case GRN_CELL_STR :
      bulk_tsv_esc(ctx, buf, obj->u.b.value, obj->u.b.size);
      *f = 1;
      break;
    case GRN_CELL_TIME :
      {
        double dv= obj->u.tv.tv_sec;
        dv += obj->u.tv.tv_nsec / GRN_TIME_NSEC_PER_SEC_F;
        grn_text_ftoa(ctx, buf, dv);
        *f = 1;
      }
      break;
    case GRN_UVECTOR :
      uvector2str(ctx, obj->u.p.value, buf);
      break;
    default :
      grn_obj_inspect(ctx, obj, buf, 0); *f = 1;
      break;
    }
  }
}

inline static grn_cell *
mk_atom(grn_ctx *ctx, char *str, unsigned int len)
{
  const char *cur, *str_end = str + len;
  int64_t ivalue = grn_atoll(str, str_end, &cur);
  if (cur == str_end) {
    grn_cell *x;
    GRN_CELL_NEW(ctx, x);
    SETINT(x, ivalue);
    return x;
  }
  switch (*str) {
  case 't' :
    if (len == 4 && !memcmp(str, "true", 4)) { return T; }
    break;
  case 'f' :
    if (len == 5 && !memcmp(str, "false", 5)) { return F; }
    break;
    /*
  case 'n' :
    if (len == 4 && !memcmp(str, "null", 4)) { return NIL; }
    break;
    */
  }
  if (0 < len && len < GRN_PAT_MAX_KEY_SIZE - 1) {
    char buf[GRN_PAT_MAX_KEY_SIZE];
    memcpy(buf, str, len);
    buf[len] = '\0';
    return INTERN(buf);
  } else {
    return F;
  }
}

inline static grn_cell *
json_readstr(grn_ctx *ctx, jctx *jc)
{
  char *start, *end;
  for (start = end = jc->cur;;) {
    unsigned int len;
    /* null check and length check */
    if (!(len = grn_charlen(ctx, end, jc->str_end))) {
      jc->cur = jc->str_end;
      break;
    }
    if (grn_isspace(end, jc->encoding)
        || *end == ':' || *end == ','
        || *end == '[' || *end == '{'
        || *end == ']' || *end == '}') {
      jc->cur = end;
      break;
    }
    end += len;
  }
  if (start < end || jc->cur < jc->str_end) {
    return mk_atom(ctx, start, end - start);
  } else {
    return F;
  }
}

inline static grn_cell *
json_readstrexp(grn_ctx *ctx, jctx *jc, int keyp)
{
  grn_cell *res;
  char *start, *src, *dest;
  for (start = src = dest = jc->cur;;) {
    unsigned int len;
    /* null check and length check */
    if (!(len = grn_charlen(ctx, src, jc->str_end))) {
      jc->cur = jc->str_end;
      if (start < dest) {
        res = keyp
          ? grn_ql_mk_symbol2(ctx, start, dest - start, 1)
          : grn_ql_mk_string(ctx, start, dest - start);
        return res ? res : F;
      }
      return F;
    }
    if (src[0] == '"' && len == 1) {
      jc->cur = src + 1;
      res = keyp
        ? grn_ql_mk_symbol2(ctx, start, dest - start, 1)
        : grn_ql_mk_string(ctx, start, dest - start);
      return res ? res : F;
    } else if (src[0] == '\\' && src + 1 < jc->str_end && len == 1) {
      src++;
      *dest++ = *src++;
    } else {
      while (len--) { *dest++ = *src++; }
    }
  }
}

static grn_cell *
json_read(grn_ctx *ctx, jctx *jc, int keyp)
{
  for (;;) {
    SKIPSPACE(jc);
    if (jc->cur >= jc->str_end) { return NULL; }
    switch (*jc->cur) {
    case '[':
      jc->cur++;
      {
        grn_cell *o, *r = NIL, **p = &r;
        while ((o = json_read(ctx, jc, 0)) && o != F) {
          *p = CONS(o, NIL);
          if (ERRP(ctx, GRN_WARN)) { return F; }
          p = &CDR(*p);
        }
        return r;
      }
    case '{':
      jc->cur++;
      {
        grn_cell *o, *r = CONS(INTERN("@"), NIL), **p = &(CDR(r));
        int i = 0;
        while ((o = json_read(ctx, jc, ((++i)&1))) && o != F) {
          *p = CONS(o, NIL);
          if (ERRP(ctx, GRN_WARN)) { return F; }
          p = &CDR(*p);
        }
        return r;
      }
    case '}':
    case ']':
      jc->cur++;
      return NULL;
    case ',':
      jc->cur++;
      break;
    case ':':
      jc->cur++;
      break;
    case '"':
      jc->cur++;
      return json_readstrexp(ctx, jc, keyp);
    default:
      return json_readstr(ctx, jc);
    }
  }
}
