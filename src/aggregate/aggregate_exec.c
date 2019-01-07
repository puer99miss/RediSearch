#include "redismodule.h"
#include "redisearch.h"
#include "search_ctx.h"
#include "aggregate.h"
#include "cursor.h"
#include "rmutil/util.h"

typedef enum { COMMAND_AGGREGATE, COMMAND_SEARCH, COMMAND_EXPLAIN } CommandType;
static void runCursor(RedisModuleCtx *outputCtx, Cursor *cursor, size_t num);
static int startCursor(AREQ *r, RedisSearchCtx *sctx, RedisModuleCtx *outctx, QueryError *err);

/**
 * Get the sorting key of the result. This will be the sorting key of the last
 * RLookup registry. Returns NULL if there is no sorting key
 */
static const RSValue *getSortKey(AREQ *req, const SearchResult *r) {
  PLN_ArrangeStep *astp = AGPLN_GetArrangeStep(&req->ap);
  if (!astp) {
    return NULL;
  }
  const RLookupKey *kk = astp->sortkeysLK[0];
  if (kk->flags & RLOOKUP_F_SVSRC) {
    return r->rowdata.sv->values[kk->svidx];
  } else {
    return RLookup_GetItem(astp->sortkeysLK[0], &r->rowdata);
  }
}

static size_t serializeResult(AREQ *req, RedisModuleCtx *outctx, const SearchResult *r) {
  const uint32_t options = req->reqflags;
  const RSDocumentMetadata *dmd = r->dmd;
  size_t count = 0;

  if (dmd && req->reqflags & QEXEC_F_IS_SEARCH) {
    size_t n;
    const char *s = DMD_KeyPtrLen(dmd, &n);
    RedisModule_ReplyWithStringBuffer(outctx, s, n);
    count++;
  }

  if (options & QEXEC_F_SEND_SCORES) {
    RedisModule_ReplyWithDouble(outctx, r->score);
    count++;
  }
  if (options & QEXEC_F_SEND_PAYLOADS) {
    count++;
    if (dmd && dmd->payload) {
      RedisModule_ReplyWithStringBuffer(outctx, dmd->payload->data, dmd->payload->len);
    } else {
      RedisModule_ReplyWithNull(outctx);
    }
  }

  if ((options & QEXEC_F_SEND_SORTKEYS)) {
    count++;
    const RSValue *sortkey = getSortKey(req, r);
    if (sortkey) {
      switch (sortkey->t) {
        case RSValue_Number:
          /* Serialize double - by prepending "%" to the number, so the coordinator/client can
           * tell it's a double and not just a numeric string value */
          RedisModule_ReplyWithString(
              outctx, RedisModule_CreateStringPrintf(outctx, "#%.17g", sortkey->numval));
          break;
        case RSValue_String:
          /* Serialize string - by prepending "$" to it */

          RedisModule_ReplyWithString(
              outctx, RedisModule_CreateStringPrintf(outctx, "$%s", sortkey->strval));
          break;
        case RSValue_RedisString:
          RedisModule_ReplyWithString(
              outctx, RedisModule_CreateStringPrintf(
                          outctx, "$%s", RedisModule_StringPtrLen(sortkey->rstrval, NULL)));
          break;
        default:
          // NIL, or any other type:
          RedisModule_ReplyWithNull(outctx);
      }
    } else {
      RedisModule_ReplyWithNull(outctx);
    }
  }

  if (!(options & QEXEC_F_SEND_NOFIELDS)) {
    count++;
    size_t nfields = 0;
    REDISMODULE_BEGIN_ARRAY(outctx);
    RLookup *lk = AGPLN_GetLookup(&req->ap, NULL, AGPLN_GETLOOKUP_LAST);

    for (const RLookupKey *kk = lk->head; kk; kk = kk->next) {
      if (kk->flags & RLOOKUP_F_HIDDEN) {
        // printf("Skipping hidden field %s/%p\n", kk->name, kk);
        continue;
      }
      nfields++;
      RedisModule_ReplyWithSimpleString(outctx, kk->name);
      const RSValue *v = RLookup_GetItem(kk, &r->rowdata);
      if (!v) {
        RedisModule_ReplyWithNull(outctx);
      } else {
        RSValue_SendReply(outctx, v);
      }
    }
    REDISMODULE_END_ARRAY(outctx, nfields * 2);
  }
  return count;
}

/**
 * Sends a chunk of <n> rows, optionally also sending the preamble
 */
static int sendChunk(AREQ *req, RedisModuleCtx *outctx, size_t limit) {
  size_t nrows = 0;
  size_t nelem = 0;
  SearchResult r = {0};
  int rc = RS_RESULT_EOF;
  ResultProcessor *rp = req->qiter.endProc;

  RedisModule_ReplyWithArray(outctx, REDISMODULE_POSTPONED_ARRAY_LEN);

  rc = rp->Next(rp, &r);
  RedisModule_ReplyWithLongLong(outctx, req->qiter.totalResults);
  nelem++;
  if (rc == RS_RESULT_OK && nrows++ < limit && !(req->reqflags & QEXEC_F_NOROWS)) {
    nelem += serializeResult(req, outctx, &r);
  }

  SearchResult_Clear(&r);
  if (rc != RS_RESULT_OK) {
    goto done;
  }

  while (nrows++ < limit && (rc = rp->Next(rp, &r)) == RS_RESULT_OK) {
    // Serialize it as a search result
    nelem += serializeResult(req, outctx, &r);
    SearchResult_Clear(&r);
  }

done:
  SearchResult_Destroy(&r);
  if (rc != RS_RESULT_OK) {
    req->stateflags |= QEXEC_S_ITERDONE;
  }
  RedisModule_ReplySetArrayLength(outctx, nelem);
  return REDISMODULE_OK;
}

void AREQ_Execute(AREQ *req, RedisModuleCtx *outctx) {
  sendChunk(req, outctx, -1);
  AREQ_Free(req);
}

static int buildRequest(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int type,
                        QueryError *status, AREQ **r) {

  int rc = REDISMODULE_ERR;
  const char *indexname = RedisModule_StringPtrLen(argv[1], NULL);
  *r = AREQ_New();
  RedisSearchCtx *sctx = NULL;
  RedisModuleCtx *thctx = NULL;

  if (type == COMMAND_SEARCH) {
    (*r)->reqflags |= QEXEC_F_IS_SEARCH;
  }

  if (AREQ_Compile(*r, argv + 2, argc - 2, status) != REDISMODULE_OK) {
    assert(QueryError_HasError(status));
    goto done;
  }

  // Prepare the query.. this is where the context is applied.
  if ((*r)->reqflags & QEXEC_F_IS_CURSOR) {
    RedisModuleCtx *newctx = RedisModule_GetThreadSafeContext(NULL);
    RedisModule_SelectDb(newctx, RedisModule_GetSelectedDb(ctx));
    ctx = thctx = newctx;  // In case of error!
  }

  sctx = NewSearchCtxC(ctx, indexname, true);
  if (!sctx) {
    QueryError_SetErrorFmt(status, QUERY_ENOINDEX, "%s: no such index", indexname);
    goto done;
  }

  if (AREQ_ApplyContext(*r, sctx, status) != REDISMODULE_OK) {
    assert(QueryError_HasError(status));
    goto done;
  }

  rc = AREQ_BuildPipeline(*r, status);

done:
  if (rc != REDISMODULE_OK && *r) {
    AREQ_Free(*r);
    *r = NULL;
    if (thctx) {
      RedisModule_FreeThreadSafeContext(thctx);
    }
  }
  return rc;
}

static int execCommandCommon(RedisModuleCtx *ctx, RedisModuleString **argv, int argc,
                             CommandType type) {
  // Index name is argv[1]
  if (argc < 2) {
    return RedisModule_WrongArity(ctx);
  }

  const char *indexname = RedisModule_StringPtrLen(argv[1], NULL);
  RedisModuleCtx *thctx = NULL;
  AREQ *r = NULL;
  QueryError status = {0};

  if (buildRequest(ctx, argv, argc, type, &status, &r) != REDISMODULE_OK) {
    goto error;
  }

  if (r->reqflags & QEXEC_F_IS_CURSOR) {
    int rc = startCursor(r, r->sctx, ctx, &status);
    if (rc != REDISMODULE_OK) {
      thctx = r->sctx->redisCtx;
      goto error;
    }
  } else {
    // Execute() will call free when appropriate.
    AREQ_Execute(r, ctx);
  }
  return REDISMODULE_OK;

error:
  if (r) {
    AREQ_Free(r);
  }
  if (thctx) {
    RedisModule_FreeThreadSafeContext(thctx);
  }

  return QueryError_ReplyAndClear(ctx, &status);
}

int RSAggregateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return execCommandCommon(ctx, argv, argc, COMMAND_AGGREGATE);
}
int RSSearchCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return execCommandCommon(ctx, argv, argc, COMMAND_SEARCH);
}

char *RS_GetExplainOutput(RedisModuleCtx *ctx, RedisModuleString **argv, int argc,
                          QueryError *status) {
  AREQ *r = NULL;
  if (buildRequest(ctx, argv, argc, COMMAND_EXPLAIN, status, &r) != REDISMODULE_OK) {
    return NULL;
  }
  char *ret = QAST_DumpExplain(&r->ast, r->sctx->spec);
  AREQ_Free(r);
  return ret;
}

static void runCursor(RedisModuleCtx *outputCtx, Cursor *cursor, size_t num);

static int startCursor(AREQ *r, RedisSearchCtx *sctx, RedisModuleCtx *outctx, QueryError *err) {
  const char *ixname = sctx->spec->name;
  Cursor *cursor = Cursors_Reserve(&RSCursors, sctx, ixname, r->cursorMaxIdle, err);
  if (cursor == NULL) {
    return REDISMODULE_ERR;
  }
  cursor->execState = r;
  cursor->sctx = sctx;
  runCursor(outctx, cursor, 0);
  return REDISMODULE_OK;
}

static void runCursor(RedisModuleCtx *outputCtx, Cursor *cursor, size_t num) {
  AREQ *req = cursor->execState;
  if (!num) {
    num = req->cursorChunkSize;
    if (!num) {
      num = RSGlobalConfig.cursorReadSize;
    }
  }
  req->cursorChunkSize = num;
  RedisModule_ReplyWithArray(outputCtx, 2);
  sendChunk(req, outputCtx, num);

  if (req->stateflags & QEXEC_S_ERROR) {
    RedisModule_ReplyWithLongLong(outputCtx, 0);
    goto delcursor;
  }

  if (req->stateflags & QEXEC_S_ITERDONE) {
    // Write the count!
    RedisModule_ReplyWithLongLong(outputCtx, 0);
  } else {
    RedisModule_ReplyWithLongLong(outputCtx, cursor->id);
  }

  if (req->stateflags & QEXEC_S_ITERDONE) {
    goto delcursor;
  } else {
    // Update the idle timeout
    Cursor_Pause(cursor);
    return;
  }

delcursor:
  AREQ_Free(req);
  if (cursor) {
    cursor->execState = NULL;
  }
  Cursor_Free(cursor);
}

/**
 * FT.CURSOR READ {index} {CID} {ROWCOUNT} [MAXIDLE]
 * FT.CURSOR DEL {index} {CID}
 * FT.CURSOR GC {index}
 */
static void cursorRead(RedisModuleCtx *ctx, uint64_t cid, size_t count) {
  Cursor *cursor = Cursors_TakeForExecution(&RSCursors, cid);
  if (cursor == NULL) {
    RedisModule_ReplyWithError(ctx, "Cursor not found");
    return;
  }
  AREQ *req = cursor->execState;
  ConcurrentSearchCtx_ReopenKeys(&req->conc);
  runCursor(ctx, cursor, count);
}

void RSCursorCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc < 4) {
    RedisModule_WrongArity(ctx);
    return;
  }

  const char *cmd = RedisModule_StringPtrLen(argv[1], NULL);
  long long cid = 0;
  // argv[1] - FT.CURSOR
  // argv[1] - subcommand
  // argv[2] - index
  // argv[3] - cursor ID

  if (RedisModule_StringToLongLong(argv[3], &cid) != REDISMODULE_OK) {
    RedisModule_ReplyWithError(ctx, "Bad cursor ID");
    return;
  }

  char cmdc = toupper(*cmd);

  if (cmdc == 'R') {
    long long count = 0;
    if (argc > 5) {
      // e.g. 'COUNT <timeout>'
      if (RedisModule_StringToLongLong(argv[5], &count) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, "Bad value for COUNT");
        return;
      }
    }
    cursorRead(ctx, cid, count);

  } else if (cmdc == 'D') {
    int rc = Cursors_Purge(&RSCursors, cid);
    if (rc != REDISMODULE_OK) {
      RedisModule_ReplyWithError(ctx, "Cursor does not exist");
    } else {
      RedisModule_ReplyWithSimpleString(ctx, "OK");
    }

  } else if (cmdc == 'G') {
    int rc = Cursors_CollectIdle(&RSCursors);
    RedisModule_ReplyWithLongLong(ctx, rc);
  } else {
    printf("Unknown command %s\n", cmd);
    RedisModule_ReplyWithError(ctx, "Unknown subcommand");
  }
}

void Cursor_FreeExecState(void *p) {
  AREQ_Free(p);
}