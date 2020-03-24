#include "rules.h"
#include "spec.h"
#include "ruledefs.h"
#include "redismodule.h"
#include "rmalloc.h"
#include "util/minmax.h"
#include "module.h"
#include "document.h"
#include "search_ctx.h"

#include <unistd.h>

/* Global list of all indexes created using `WITHRULES` */
static IndexSpec **rindexes_g = NULL;

SchemaRules *SchemaRules_g = NULL;
AsyncIndexQueue *asyncQueue_g = NULL;

SchemaRules *SchemaRules_Create(void) {
  SchemaRules *rules = rm_calloc(1, sizeof(*rules));
  rules->rules = array_new(SchemaRule *, 8);
  rules->actions = array_new(MatchAction, 1);
  return rules;
}

RSAddDocumentCtx *SchemaRules_InitACTX(RedisModuleCtx *ctx, IndexSpec *sp, RuleKeyItem *item,
                                       const IndexItemAttrs *attrs, QueryError *e) {
  Document d = {0};
  RedisSearchCtx sctx = SEARCH_CTX_STATIC(ctx, sp);

  RSAddDocumentCtx *aCtx = NULL;
  Document_Init(&d, item->kstr, attrs->score, attrs->language);
  d.keyobj = item->kobj;
  int rv = Document_LoadSchemaFields(&d, &sctx, e);
  if (rv != REDISMODULE_OK) {
    goto err;
  }

  aCtx = ACTX_New(sp, &d, e);
  if (!aCtx) {
    goto err;
  }
  return aCtx;

err:
  Document_Free(&d);
  return NULL;
}

int SchemaRules_IndexDocument(RedisModuleCtx *ctx, IndexSpec *sp, RuleKeyItem *item,
                              const IndexItemAttrs *attrs, QueryError *e) {
  RedisSearchCtx sctx = SEARCH_CTX_STATIC(ctx, sp);
  RSAddDocumentCtx *aCtx = SchemaRules_InitACTX(ctx, sp, item, attrs, e);
  if (!aCtx) {
    return REDISMODULE_ERR;
  }
  ACTX_Index(aCtx, &sctx, DOCUMENT_ADD_REPLACE);
  assert(QueryError_HasError(&aCtx->status) == 0);
  ACTX_Free(aCtx);
  return REDISMODULE_OK;
}

void SchemaRules_ProcessItem(RedisModuleCtx *ctx, RuleKeyItem *item, int flags) {
  /**
   * Inspect the key, see which indexes match the key, and then perform the appropriate actions,
   * maybe in a different thread?
   */
  MatchAction *results = NULL;
  size_t nresults = 0;
  SchemaRules_Check(SchemaRules_g, ctx, item, &results, &nresults);

  for (size_t ii = 0; ii < nresults; ++ii) {
    // submit the document for indexing if sync, async otherwise...
    IndexSpec *spec = IndexSpec_Load(ctx, results[ii].index, 1);
    assert(spec);  // todo handle error...

    if ((flags & RULES_PROCESS_F_NOREINDEX) && DocTable_GetByKeyR(&spec->docs, item->kstr)) {
      // in SCAN mode and document already exists in the index
      continue;
    }

    // check if spec uses synchronous or asynchronous indexing..
    if ((flags & RULES_PROCESS_F_ASYNC) || (spec->flags & Index_Async)) {
      AIQ_Submit(asyncQueue_g, spec, results + ii, item);
    } else {
      QueryError e = {0};
      int rc = SchemaRules_IndexDocument(ctx, spec, item, &results[ii].attrs, &e);
      if (rc != REDISMODULE_OK) {
        printf("Couldn't index document: %s\n", QueryError_GetError(&e));
        if (e.code == QUERY_ENOIDXFIELDS) {
          rc = REDISMODULE_OK;
        }
        QueryError_ClearError(&e);
      }
      assert(rc == REDISMODULE_OK);
    }
  }
}

static int hashCallback(RedisModuleCtx *ctx, int unused, const char *action,
                        RedisModuleString *key) {
  RuleKeyItem item = {.kstr = key, .kobj = NULL};
  SchemaRules_ProcessItem(ctx, &item, 0);
  if (item.kobj) {
    RedisModule_CloseKey(item.kobj);
  }
  return REDISMODULE_OK;
}

static int delCallback(RedisModuleCtx *ctx, int event, const char *action,
                       RedisModuleString *keyname) {
  int shouldDelete = 0;
  if (event & (REDISMODULE_NOTIFY_EVICTED | REDISMODULE_NOTIFY_EXPIRED)) {
    shouldDelete = 1;
  } else if (event == REDISMODULE_NOTIFY_GENERIC && *action == 'd') {
    shouldDelete = 1;
  }
  if (!shouldDelete) {
    return REDISMODULE_OK;
  }

  // If it's a delete, broadcast the key to all 'WITHRULES' indexes
  size_t nidx = array_len(rindexes_g);
  for (size_t ii = 0; ii < array_len(rindexes_g); ++ii) {
    IndexSpec *sp = rindexes_g[ii];
    DocTable_DeleteR(&sp->docs, keyname);
    if (sp->flags & Index_Async) {
    }
  }

  // also, if the index is async, remove it from all async queues
  return REDISMODULE_OK;
}

void SchemaRules_ReplyAll(const SchemaRules *rules, RedisModuleCtx *ctx) {
}

void SchemaRules_InitGlobal(RedisModuleCtx *ctx) {
  asyncQueue_g = AIQ_Create(1000, 5);

  SchemaRules_g = SchemaRules_Create();
  RedisModule_SubscribeToKeyspaceEvents(RSDummyContext, REDISMODULE_NOTIFY_HASH, hashCallback);
  RedisModule_SubscribeToKeyspaceEvents(
      RSDummyContext, REDISMODULE_NOTIFY_GENERIC | REDISMODULE_NOTIFY_EXPIRED, delCallback);
}

void SchemaRules_ShutdownGlobal() {
  AIQ_Destroy(asyncQueue_g);
  asyncQueue_g = NULL;
}

void SchemaRules_RegisterIndex(IndexSpec *sp) {
  if (!rindexes_g) {
    rindexes_g = array_new(IndexSpec *, 1);
  }
  rindexes_g = array_append(rindexes_g, sp);
}

void SchemaRules_UnregisterIndex(IndexSpec *sp) {
  if (!rindexes_g) {
    return;
  }

  size_t ix;
  for (ix = 0; ix < array_len(rindexes_g); ++ix) {
    if (rindexes_g[ix] == sp) {
      break;
    }
  }
  if (ix == array_len(rindexes_g)) {
    return;
  }
  rindexes_g = array_del_fast(rindexes_g, ix);
  // Iterate through all the rules which reference this index
  SchemaRules *rules = SchemaRules_g;
  for (size_t ii = 0; ii < array_len(rules->rules); ++ii) {
    if (strcasecmp(rules->rules[ii]->index, sp->name)) {
      continue;
    }
    ARRAY_DEL_ITER(rules->rules, ii);
  }
}

#define RULES_CURRENT_VERSION 0

/**
 * FORMAT:
 * nrules (int)
 *  index (str)
 *  name (str)
 *  nargs (int)
 *   arg...
 */

static void rulesAuxSave(RedisModuleIO *rdb, int when) {
  if (when != REDISMODULE_AUX_BEFORE_RDB) {
    return;
  }
  SchemaRules *rules = SchemaRules_g;
  RedisModule_SaveUnsigned(rdb, array_len(rules->rules));
  DLLIST_node *nn;
  size_t nrules = array_len(rules->rules);
  for (size_t ii = 0; ii < nrules; ++ii) {
    SchemaRule *r = rules->rules[ii];
    size_t n = array_len(r->rawrule);
    RedisModule_SaveStringBuffer(rdb, r->index, strlen(r->index));
    RedisModule_SaveStringBuffer(rdb, r->name, strlen(r->name));
    RedisModule_SaveUnsigned(rdb, n);
    for (size_t jj = 0; jj < n; ++jj) {
      const char *s = r->rawrule[jj];
      RedisModule_SaveStringBuffer(rdb, s, strlen(s));
    }
  }
}

static int rulesAuxLoad(RedisModuleIO *rdb, int encver, int when) {
  // Going to assume that the rules are already loaded..
  if (encver > RULES_CURRENT_VERSION) {
    return REDISMODULE_ERR;
  }
  if (when != REDISMODULE_AUX_BEFORE_RDB) {
    return REDISMODULE_OK;
  }

  SchemaRules *rules = SchemaRules_g;

  size_t nrules = RedisModule_LoadUnsigned(rdb);
  for (size_t ii = 0; ii < nrules; ++ii) {
    size_t ns;
    RedisModuleString *index = RedisModule_LoadString(rdb);
    RedisModuleString *name = RedisModule_LoadString(rdb);
    size_t nargs = RedisModule_LoadUnsigned(rdb);
    RedisModuleString **args = rm_malloc(sizeof(*args) * nargs);
    for (size_t jj = 0; jj < nargs; ++jj) {
      args[jj] = RedisModule_LoadString(rdb);
    }
    ArgsCursor ac = {0};
    ArgsCursor_InitRString(&ac, args, nargs);
    QueryError status = {0};
    int rc = SchemaRules_AddArgs(rules, RedisModule_StringPtrLen(index, NULL),
                                 RedisModule_StringPtrLen(name, NULL), &ac, &status);
    if (rc != REDISMODULE_OK) {
      printf("Couldn't load rules: %s\n", QueryError_GetError(&status));
    }
    RedisModule_FreeString(NULL, index);
    RedisModule_FreeString(NULL, name);
    for (size_t jj = 0; jj < nargs; ++jj) {
      RedisModule_FreeString(NULL, args[jj]);
    }
    rm_free(args);
    if (rc != REDISMODULE_OK) {
      return REDISMODULE_ERR;
    }
  }
  return REDISMODULE_OK;
}

int SchemaRules_RegisterType(RedisModuleCtx *ctx) {
  RedisModuleTypeMethods m = {
      .version = REDISMODULE_TYPE_METHOD_VERSION,
      .aux_load = rulesAuxLoad,
      .aux_save = rulesAuxSave,
      .aux_save_triggers = REDISMODULE_AUX_BEFORE_RDB | REDISMODULE_AUX_AFTER_RDB};
  RedisModuleType *t = RedisModule_CreateDataType(ctx, "ft_rules0", RULES_CURRENT_VERSION, &m);
  return t ? REDISMODULE_OK : REDISMODULE_ERR;
}