/*
** 2003 April 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to implement the ATTACH and DETACH commands.
*/
#if defined(SQLITE_BUILDING_FOR_COMDB2)
#include <string.h>
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
#include "sqliteInt.h"

#if defined(SQLITE_BUILDING_FOR_COMDB2)
#include "logmsg.h"
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

#ifndef SQLITE_OMIT_ATTACH
/*
** Resolve an expression that was part of an ATTACH or DETACH statement. This
** is slightly different from resolving a normal SQL expression, because simple
** identifiers are treated as strings, not possible column names or aliases.
**
** i.e. if the parser sees:
**
**     ATTACH DATABASE abc AS def
**
** it treats the two expressions as literal strings 'abc' and 'def' instead of
** looking for columns of the same name.
**
** This only applies to the root node of pExpr, so the statement:
**
**     ATTACH DATABASE abc||def AS 'db2'
**
** will fail because neither abc or def can be resolved.
*/
static int resolveAttachExpr(NameContext *pName, Expr *pExpr)
{
  int rc = SQLITE_OK;
  if( pExpr ){
    if( pExpr->op!=TK_ID ){
      rc = sqlite3ResolveExprNames(pName, pExpr);
    }else{
      pExpr->op = TK_STRING;
    }
  }
  return rc;
}

/*
** An SQL user-function registered to do the work of an ATTACH statement. The
** three arguments to the function come directly from an attach statement:
**
**     ATTACH DATABASE x AS y KEY z
**
**     SELECT sqlite_attach(x, y, z)
**
** If the optional "KEY z" syntax is omitted, an SQL NULL is passed as the
** third argument.
**
** If the db->init.reopenMemdb flags is set, then instead of attaching a
** new database, close the database on db->init.iDb and reopen it as an
** empty MemDB.
*/
#if defined(SQLITE_BUILDING_FOR_COMDB2)
int comdb2_dynamic_attach(
  sqlite3 *db,
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
static void attachFunc(
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  sqlite3_context *context,
  int NotUsed,
  sqlite3_value **argv
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  ,const char *zName,
  const char *zFile,
  char **pzErrDyn,
  int version,
  int class,
  int local,
  int class_override,
  int proto_version
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
){
  int i;
  int rc = 0;
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *zName;
  const char *zFile;
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
  char *zPath = 0;
  char *zErr = 0;
  unsigned int flags;
  Db *aNew;                 /* New array of Db pointers */
  Db *pNew;                 /* Db object for the newly attached database */
  char *zErrDyn = 0;
  sqlite3_vfs *pVfs;

#if defined(SQLITE_BUILDING_FOR_COMDB2)
  char *dbName;
  char *tblName;
  int  iFndDb;
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  UNUSED_PARAMETER(NotUsed);
  zFile = (const char *)sqlite3_value_text(argv[0]);
  zName = (const char *)sqlite3_value_text(argv[1]);
  if( zFile==0 ) zFile = "";
  if( zName==0 ) zName = "";
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

#ifdef SQLITE_ENABLE_DESERIALIZE
# define REOPEN_AS_MEMDB(db)  (db->init.reopenMemdb)
#else
# define REOPEN_AS_MEMDB(db)  (0)
#endif

  if( REOPEN_AS_MEMDB(db) ){
    /* This is not a real ATTACH.  Instead, this routine is being called
    ** from sqlite3_deserialize() to close database db->init.iDb and
    ** reopen it as a MemDB */
    pVfs = sqlite3_vfs_find("memdb");
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    if( pVfs==0 ){
      zErrDyn = sqlite3MPrintf(db,
                               "%s: the \"memdb\" VFS was not found\n",
                               __func__);
      rc = SQLITE_ERROR;
      goto attach_error;
    }
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    if( pVfs==0 ) return;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    pNew = &db->aDb[db->init.iDb];
    if( pNew->pBt ) sqlite3BtreeClose(pNew->pBt);
    pNew->pBt = 0;
    pNew->pSchema = 0;
    rc = sqlite3BtreeOpen(pVfs, "x\0", db, &pNew->pBt, 0, SQLITE_OPEN_MAIN_DB);
  }else{
    /* This is a real ATTACH
    **
    ** Check for the following errors:
    **
    **     * Too many attached databases,
    **     * Transaction currently open
    **     * Specified database name already being used.
    */
    if( db->nDb>=db->aLimit[SQLITE_LIMIT_ATTACHED]+2 ){
      zErrDyn = sqlite3MPrintf(db, "too many attached databases - max %d", 
        db->aLimit[SQLITE_LIMIT_ATTACHED]
      );
      goto attach_error;
    }
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    /* we store one Btree per foreign db, therefore we need to extract
    ** the dbname from table */
    dbName = sqlite3DbStrDup(db, zName);
    if( dbName==0 ){
      rc = SQLITE_NOMEM_BKPT;
      goto attach_error;
    }else{
      /* to keep table names unique, zName includes the dbname as well.
      ** But the Btree will be named after dbname, since they will collect all
      ** the tables for this db */
      char *ptr = strchr(dbName, '.');
      if( ptr ){
        *ptr = '\0';
        tblName = (ptr+1);
      }else{
        logmsg(LOGMSG_ERROR, "%s: remote dbname URI incomplete \"%s\"?\n",
               __func__, dbName);
        zErrDyn = sqlite3MPrintf(db,
                                 "%s: remote dbname URI incomplete \"%s\"?\n",
                                 __func__, dbName);
        sqlite3DbFree(db, dbName);

        rc = SQLITE_ERROR;
        goto attach_error;
      }
    }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    for(i=0; i<db->nDb; i++){
      char *z = db->aDb[i].zDbSName;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
      assert( z && dbName);
      if( sqlite3StrICmp(z, dbName)==0 ){
        break;
      }
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      assert( z && zName );
      if( sqlite3StrICmp(z, zName)==0 ){
        zErrDyn = sqlite3MPrintf(db, "database %s is already in use", zName);
        goto attach_error;
      }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    }
  
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    iFndDb = i;
    if( i!=db->nDb ) {
      extern int sqlite3BtreeReopen(const char*,Btree*);

      rc = sqlite3BtreeReopen(dbName, db->aDb[iFndDb].pBt);

      db->aDb[iFndDb].zDbSName = dbName;
      db->aDb[iFndDb].class = class;
      db->aDb[iFndDb].class_override = class_override;
      db->aDb[iFndDb].local = local;
      db->aDb[iFndDb].version = proto_version;
      goto done_with_open;
    }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    /* Allocate the new entry in the db->aDb[] array and initialize the schema
    ** hash tables.
    */
    if( db->aDb==db->aDbStatic ){
      aNew = sqlite3DbMallocRawNN(db, sizeof(db->aDb[0])*3 );
#if defined(SQLITE_BUILDING_FOR_COMDB2)
      if( aNew==0 ) return -1;
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      if( aNew==0 ) return;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      memcpy(aNew, db->aDb, sizeof(db->aDb[0])*2);
    }else{
      aNew = sqlite3DbRealloc(db, db->aDb, sizeof(db->aDb[0])*(db->nDb+1) );
#if defined(SQLITE_BUILDING_FOR_COMDB2)
      if( aNew==0 ) return -1;
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      if( aNew==0 ) return;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    }
    db->aDb = aNew;
    pNew = &db->aDb[db->nDb];
    memset(pNew, 0, sizeof(*pNew));
  
    /* Open the database file. If the btree is successfully opened, use
    ** it to obtain the database schema. At this point the schema may
    ** or may not be initialized.
    */
    flags = db->openFlags;
    rc = sqlite3ParseUri(db->pVfs->zName, zFile, &flags, &pVfs, &zPath, &zErr);
    if( rc!=SQLITE_OK ){
      if( rc==SQLITE_NOMEM ) sqlite3OomFault(db);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
      if( context )
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      sqlite3_result_error(context, zErr, -1);
      sqlite3_free(zErr);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
      return -1;
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      return;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    }
    assert( pVfs );
    flags |= SQLITE_OPEN_MAIN_DB;
    rc = sqlite3BtreeOpen(pVfs, zPath, db, &pNew->pBt, 0, flags);
    db->nDb++;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    pNew->zDbSName = dbName;
    pNew->class = class;
    pNew->class_override = class_override;
    pNew->local = local;
    pNew->version = proto_version;
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    pNew->zDbSName = sqlite3DbStrDup(db, zName);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  }
  db->noSharedCache = 0;
  if( rc==SQLITE_CONSTRAINT ){
    rc = SQLITE_ERROR;
    zErrDyn = sqlite3MPrintf(db, "database is already attached");
  }else if( rc==SQLITE_OK ){
    Pager *pPager;
    pNew->pSchema = sqlite3SchemaGet(db, pNew->pBt);
    if( !pNew->pSchema ){
      rc = SQLITE_NOMEM_BKPT;
    }else if( pNew->pSchema->file_format && pNew->pSchema->enc!=ENC(db) ){
      zErrDyn = sqlite3MPrintf(db, 
        "attached databases must use the same text encoding as main database");
      rc = SQLITE_ERROR;
    }
    sqlite3BtreeEnter(pNew->pBt);
    pPager = sqlite3BtreePager(pNew->pBt);
    sqlite3PagerLockingMode(pPager, db->dfltLockMode);
    sqlite3BtreeSecureDelete(pNew->pBt,
                             sqlite3BtreeSecureDelete(db->aDb[0].pBt,-1) );
#ifndef SQLITE_OMIT_PAGER_PRAGMAS
    sqlite3BtreeSetPagerFlags(pNew->pBt,
                      PAGER_SYNCHRONOUS_FULL | (db->flags & PAGER_FLAGS_MASK));
#endif
    sqlite3BtreeLeave(pNew->pBt);
  }
  pNew->safety_level = SQLITE_DEFAULT_SYNCHRONOUS+1;
  if( rc==SQLITE_OK && pNew->zDbSName==0 ){
    rc = SQLITE_NOMEM_BKPT;
  }


#ifdef SQLITE_HAS_CODEC
  if( rc==SQLITE_OK ){
    extern int sqlite3CodecAttach(sqlite3*, int, const void*, int);
    extern void sqlite3CodecGetKey(sqlite3*, int, void**, int*);
    int nKey;
    char *zKey;
    int t = sqlite3_value_type(argv[2]);
    switch( t ){
      case SQLITE_INTEGER:
      case SQLITE_FLOAT:
        zErrDyn = sqlite3DbStrDup(db, "Invalid key value");
        rc = SQLITE_ERROR;
        break;
        
      case SQLITE_TEXT:
      case SQLITE_BLOB:
        nKey = sqlite3_value_bytes(argv[2]);
        zKey = (char *)sqlite3_value_blob(argv[2]);
        rc = sqlite3CodecAttach(db, db->nDb-1, zKey, nKey);
        break;

      case SQLITE_NULL:
        /* No key specified.  Use the key from URI filename, or if none,
        ** use the key from the main database. */
        if( sqlite3CodecQueryParameters(db, zName, zPath)==0 ){
          sqlite3CodecGetKey(db, 0, (void**)&zKey, &nKey);
          if( nKey || sqlite3BtreeGetOptimalReserve(db->aDb[0].pBt)>0 ){
            rc = sqlite3CodecAttach(db, db->nDb-1, zKey, nKey);
          }
        }
        break;
    }
  }
#endif
#if defined(SQLITE_BUILDING_FOR_COMDB2)
done_with_open:
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  sqlite3_free( zPath );

  /* If the file was opened successfully, read the schema for the new database.
  ** If this fails, or if opening the file failed, then close the file and 
  ** remove the entry from the db->aDb[] array. i.e. put everything back the
  ** way we found it.
  */
  if( rc==SQLITE_OK ){
    sqlite3BtreeEnterAll(db);
    db->init.iDb = 0;
    db->mDbFlags &= ~(DBFLAG_SchemaKnownOk);
    if( !REOPEN_AS_MEMDB(db) ){
#if defined(SQLITE_BUILDING_FOR_COMDB2)
      Table *p;
      int savedBusy = db->init.busy;

      db->init.busy = 0; /* TODO: prevent assert (?) */
      rc = sqlite3InitTable(db, &zErrDyn, zName);
      db->init.busy = savedBusy;

      /*
      ** Need to set the version to the table to support per table schema
      ** refresh
      */
      p = sqlite3HashFind(&db->aDb[iFndDb].pSchema->tblHash, tblName);
      if( !p ){
        logmsg(LOGMSG_ERROR, "%s: failed to find table \"%s\" after init\n",
               __func__, tblName);
        rc = SQLITE_ERROR;
      }else{
        p->version = version;
        p->iDb = iFndDb;
      }
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      rc = sqlite3Init(db, &zErrDyn);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    }
    sqlite3BtreeLeaveAll(db);
    assert( zErrDyn==0 || rc!=SQLITE_OK );
  }
#ifdef SQLITE_USER_AUTHENTICATION
  if( rc==SQLITE_OK && !REOPEN_AS_MEMDB(db) ){
    u8 newAuth = 0;
    rc = sqlite3UserAuthCheckLogin(db, zName, &newAuth);
    if( newAuth<db->auth.authLevel ){
      rc = SQLITE_AUTH_USER;
    }
  }
#endif
  if( rc ){
    if( !REOPEN_AS_MEMDB(db) ){
      int iDb = db->nDb - 1;
      assert( iDb>=2 );
      if( db->aDb[iDb].pBt ){
        sqlite3BtreeClose(db->aDb[iDb].pBt);
        db->aDb[iDb].pBt = 0;
        db->aDb[iDb].pSchema = 0;
      }
      sqlite3ResetAllSchemasOfConnection(db);
      db->nDb = iDb;
      if( rc==SQLITE_NOMEM || rc==SQLITE_IOERR_NOMEM ){
        sqlite3OomFault(db);
        sqlite3DbFree(db, zErrDyn);
        zErrDyn = sqlite3MPrintf(db, "out of memory");
      }else if( zErrDyn==0 ){
        zErrDyn = sqlite3MPrintf(db, "unable to open database: %s", zFile);
      }
    }
    goto attach_error;
  }
  
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  return 0;
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  return;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

attach_error:
  /* Return an error if we get here */
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  *pzErrDyn = zErrDyn;
  return rc;
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  if( zErrDyn ){
    sqlite3_result_error(context, zErrDyn, -1);
    sqlite3DbFree(db, zErrDyn);
  }
  if( rc ) sqlite3_result_error_code(context, rc);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
}

#if defined(SQLITE_BUILDING_FOR_COMDB2)
static void attachFunc(
  sqlite3_context *context,
  int NotUsed,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(context);
  const char *zName;
  const char *zFile;
  char *zErrDyn;
  int rc = 0;

  UNUSED_PARAMETER(NotUsed);

  zFile = (const char *)sqlite3_value_text(argv[0]);
  zName = (const char *)sqlite3_value_text(argv[1]);
  if( zFile==0 ) zFile = "";
  if( zName==0 ) zName = "";

  zErrDyn = NULL;
  rc = comdb2_dynamic_attach(db, context, 0, argv, zName, zFile, &zErrDyn, 0, 0, 0, 0, 0);
  if( zErrDyn ){
    sqlite3_result_error(context, zErrDyn, -1);
    sqlite3DbFree(db, zErrDyn);
  }
  if( rc ) sqlite3_result_error_code(context, rc);
}

void comdb2_dynamic_detach(sqlite3 *db, int idx)
{
  Db *pDb = &db->aDb[idx];
  sqlite3BtreeClose(pDb->pBt);
  pDb->pBt = 0;
  pDb->pSchema = 0;
  sqlite3CollapseDatabaseArray(db);
}
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

/*
** An SQL user-function registered to do the work of an DETACH statement. The
** three arguments to the function come directly from a detach statement:
**
**     DETACH DATABASE x
**
**     SELECT sqlite_detach(x)
*/
static void detachFunc(
  sqlite3_context *context,
  int NotUsed,
  sqlite3_value **argv
){
  const char *zName = (const char *)sqlite3_value_text(argv[0]);
  sqlite3 *db = sqlite3_context_db_handle(context);
  int i;
  Db *pDb = 0;
  char zErr[128];

  UNUSED_PARAMETER(NotUsed);

  if( zName==0 ) zName = "";
  for(i=0; i<db->nDb; i++){
    pDb = &db->aDb[i];
    if( pDb->pBt==0 ) continue;
    if( sqlite3StrICmp(pDb->zDbSName, zName)==0 ) break;
  }

  if( i>=db->nDb ){
    sqlite3_snprintf(sizeof(zErr),zErr, "no such database: %s", zName);
    goto detach_error;
  }
  if( i<2 ){
    sqlite3_snprintf(sizeof(zErr),zErr, "cannot detach database %s", zName);
    goto detach_error;
  }
  if( sqlite3BtreeIsInReadTrans(pDb->pBt) || sqlite3BtreeIsInBackup(pDb->pBt) ){
    sqlite3_snprintf(sizeof(zErr),zErr, "database %s is locked", zName);
    goto detach_error;
  }

#if defined(SQLITE_BUILDING_FOR_COMDB2)
  comdb2_dynamic_detach(db, i);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  sqlite3BtreeClose(pDb->pBt);
  pDb->pBt = 0;
  pDb->pSchema = 0;
  sqlite3CollapseDatabaseArray(db);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  return;

detach_error:
  sqlite3_result_error(context, zErr, -1);
}

/*
** This procedure generates VDBE code for a single invocation of either the
** sqlite_detach() or sqlite_attach() SQL user functions.
*/
static void codeAttach(
  Parse *pParse,       /* The parser context */
  int type,            /* Either SQLITE_ATTACH or SQLITE_DETACH */
  FuncDef const *pFunc,/* FuncDef wrapper for detachFunc() or attachFunc() */
  Expr *pAuthArg,      /* Expression to pass to authorization callback */
  Expr *pFilename,     /* Name of database file */
  Expr *pDbname,       /* Name of the database to use internally */
  Expr *pKey           /* Database key for encryption extension */
){
  int rc;
  NameContext sName;
  Vdbe *v;
  sqlite3* db = pParse->db;
  int regArgs;

  if( pParse->nErr ) goto attach_end;
  memset(&sName, 0, sizeof(NameContext));
  sName.pParse = pParse;

  if( 
      SQLITE_OK!=(rc = resolveAttachExpr(&sName, pFilename)) ||
      SQLITE_OK!=(rc = resolveAttachExpr(&sName, pDbname)) ||
      SQLITE_OK!=(rc = resolveAttachExpr(&sName, pKey))
  ){
    goto attach_end;
  }

#ifndef SQLITE_OMIT_AUTHORIZATION
  if( pAuthArg ){
    char *zAuthArg;
    if( pAuthArg->op==TK_STRING ){
      zAuthArg = pAuthArg->u.zToken;
    }else{
      zAuthArg = 0;
    }
    rc = sqlite3AuthCheck(pParse, type, zAuthArg, 0, 0);
    if(rc!=SQLITE_OK ){
      goto attach_end;
    }
  }
#endif /* SQLITE_OMIT_AUTHORIZATION */


  v = sqlite3GetVdbe(pParse);
  regArgs = sqlite3GetTempRange(pParse, 4);
  sqlite3ExprCode(pParse, pFilename, regArgs);
  sqlite3ExprCode(pParse, pDbname, regArgs+1);
  sqlite3ExprCode(pParse, pKey, regArgs+2);

  assert( v || db->mallocFailed );
  if( v ){
    sqlite3VdbeAddOp4(v, OP_Function0, 0, regArgs+3-pFunc->nArg, regArgs+3,
                      (char *)pFunc, P4_FUNCDEF);
    assert( pFunc->nArg==-1 || (pFunc->nArg&0xff)==pFunc->nArg );
    sqlite3VdbeChangeP5(v, (u8)(pFunc->nArg));
 
    /* Code an OP_Expire. For an ATTACH statement, set P1 to true (expire this
    ** statement only). For DETACH, set it to false (expire all existing
    ** statements).
    */
    sqlite3VdbeAddOp1(v, OP_Expire, (type==SQLITE_ATTACH));
  }
  
attach_end:
  sqlite3ExprDelete(db, pFilename);
  sqlite3ExprDelete(db, pDbname);
  sqlite3ExprDelete(db, pKey);
}

/*
** Called by the parser to compile a DETACH statement.
**
**     DETACH pDbname
*/
void sqlite3Detach(Parse *pParse, Expr *pDbname){
  static const FuncDef detach_func = {
    1,                /* nArg */
    SQLITE_UTF8,      /* funcFlags */
    0,                /* pUserData */
    0,                /* pNext */
    detachFunc,       /* xSFunc */
    0,                /* xFinalize */
    0, 0,             /* xValue, xInverse */
    "sqlite_detach",  /* zName */
    {0}
  };
  codeAttach(pParse, SQLITE_DETACH, &detach_func, pDbname, 0, 0, pDbname);
}

/*
** Called by the parser to compile an ATTACH statement.
**
**     ATTACH p AS pDbname KEY pKey
*/
void sqlite3Attach(Parse *pParse, Expr *p, Expr *pDbname, Expr *pKey){
  static const FuncDef attach_func = {
    3,                /* nArg */
    SQLITE_UTF8,      /* funcFlags */
    0,                /* pUserData */
    0,                /* pNext */
    attachFunc,       /* xSFunc */
    0,                /* xFinalize */
    0, 0,             /* xValue, xInverse */
    "sqlite_attach",  /* zName */
    {0}
  };
  codeAttach(pParse, SQLITE_ATTACH, &attach_func, p, p, pDbname, pKey);
}
#endif /* SQLITE_OMIT_ATTACH */

/*
** Initialize a DbFixer structure.  This routine must be called prior
** to passing the structure to one of the sqliteFixAAAA() routines below.
*/
void sqlite3FixInit(
  DbFixer *pFix,      /* The fixer to be initialized */
  Parse *pParse,      /* Error messages will be written here */
  int iDb,            /* This is the database that must be used */
  const char *zType,  /* "view", "trigger", or "index" */
  const Token *pName  /* Name of the view, trigger, or index */
){
  sqlite3 *db;

  db = pParse->db;
  assert( db->nDb>iDb );
  pFix->pParse = pParse;
  pFix->zDb = db->aDb[iDb].zDbSName;
  pFix->pSchema = db->aDb[iDb].pSchema;
  pFix->zType = zType;
  pFix->pName = pName;
  pFix->bVarOnly = (iDb==1);
}

/*
** The following set of routines walk through the parse tree and assign
** a specific database to all table references where the database name
** was left unspecified in the original SQL statement.  The pFix structure
** must have been initialized by a prior call to sqlite3FixInit().
**
** These routines are used to make sure that an index, trigger, or
** view in one database does not refer to objects in a different database.
** (Exception: indices, triggers, and views in the TEMP database are
** allowed to refer to anything.)  If a reference is explicitly made
** to an object in a different database, an error message is added to
** pParse->zErrMsg and these routines return non-zero.  If everything
** checks out, these routines return 0.
*/
int sqlite3FixSrcList(
  DbFixer *pFix,       /* Context of the fixation */
  SrcList *pList       /* The Source list to check and modify */
){
  int i;
  const char *zDb;
  struct SrcList_item *pItem;

  if( NEVER(pList==0) ) return 0;
  zDb = pFix->zDb;
  for(i=0, pItem=pList->a; i<pList->nSrc; i++, pItem++){
    if( pFix->bVarOnly==0 ){
      if( pItem->zDatabase && sqlite3StrICmp(pItem->zDatabase, zDb) ){
        sqlite3ErrorMsg(pFix->pParse,
            "%s %T cannot reference objects in database %s",
            pFix->zType, pFix->pName, pItem->zDatabase);
        return 1;
      }
      sqlite3DbFree(pFix->pParse->db, pItem->zDatabase);
      pItem->zDatabase = 0;
      pItem->pSchema = pFix->pSchema;
    }
#if !defined(SQLITE_OMIT_VIEW) || !defined(SQLITE_OMIT_TRIGGER)
    if( sqlite3FixSelect(pFix, pItem->pSelect) ) return 1;
    if( sqlite3FixExpr(pFix, pItem->pOn) ) return 1;
#endif
    if( pItem->fg.isTabFunc && sqlite3FixExprList(pFix, pItem->u1.pFuncArg) ){
      return 1;
    }
  }
  return 0;
}
#if !defined(SQLITE_OMIT_VIEW) || !defined(SQLITE_OMIT_TRIGGER)
int sqlite3FixSelect(
  DbFixer *pFix,       /* Context of the fixation */
  Select *pSelect      /* The SELECT statement to be fixed to one database */
){
  while( pSelect ){
    if( sqlite3FixExprList(pFix, pSelect->pEList) ){
      return 1;
    }
    if( sqlite3FixSrcList(pFix, pSelect->pSrc) ){
      return 1;
    }
    if( sqlite3FixExpr(pFix, pSelect->pWhere) ){
      return 1;
    }
    if( sqlite3FixExprList(pFix, pSelect->pGroupBy) ){
      return 1;
    }
    if( sqlite3FixExpr(pFix, pSelect->pHaving) ){
      return 1;
    }
    if( sqlite3FixExprList(pFix, pSelect->pOrderBy) ){
      return 1;
    }
    if( sqlite3FixExpr(pFix, pSelect->pLimit) ){
      return 1;
    }
    if( pSelect->pWith ){
      int i;
      for(i=0; i<pSelect->pWith->nCte; i++){
        if( sqlite3FixSelect(pFix, pSelect->pWith->a[i].pSelect) ){
          return 1;
        }
      }
    }
    pSelect = pSelect->pPrior;
  }
  return 0;
}
int sqlite3FixExpr(
  DbFixer *pFix,     /* Context of the fixation */
  Expr *pExpr        /* The expression to be fixed to one database */
){
  while( pExpr ){
    if( pExpr->op==TK_VARIABLE ){
      if( pFix->pParse->db->init.busy ){
        pExpr->op = TK_NULL;
      }else{
        sqlite3ErrorMsg(pFix->pParse, "%s cannot use variables", pFix->zType);
        return 1;
      }
    }
    if( ExprHasProperty(pExpr, EP_TokenOnly|EP_Leaf) ) break;
    if( ExprHasProperty(pExpr, EP_xIsSelect) ){
      if( sqlite3FixSelect(pFix, pExpr->x.pSelect) ) return 1;
    }else{
      if( sqlite3FixExprList(pFix, pExpr->x.pList) ) return 1;
    }
    if( sqlite3FixExpr(pFix, pExpr->pRight) ){
      return 1;
    }
    pExpr = pExpr->pLeft;
  }
  return 0;
}
int sqlite3FixExprList(
  DbFixer *pFix,     /* Context of the fixation */
  ExprList *pList    /* The expression to be fixed to one database */
){
  int i;
  struct ExprList_item *pItem;
  if( pList==0 ) return 0;
  for(i=0, pItem=pList->a; i<pList->nExpr; i++, pItem++){
    if( sqlite3FixExpr(pFix, pItem->pExpr) ){
      return 1;
    }
  }
  return 0;
}
#endif

#ifndef SQLITE_OMIT_TRIGGER
int sqlite3FixTriggerStep(
  DbFixer *pFix,     /* Context of the fixation */
  TriggerStep *pStep /* The trigger step be fixed to one database */
){
  while( pStep ){
    if( sqlite3FixSelect(pFix, pStep->pSelect) ){
      return 1;
    }
    if( sqlite3FixExpr(pFix, pStep->pWhere) ){
      return 1;
    }
    if( sqlite3FixExprList(pFix, pStep->pExprList) ){
      return 1;
    }
#ifndef SQLITE_OMIT_UPSERT
    if( pStep->pUpsert ){
      Upsert *pUp = pStep->pUpsert;
      if( sqlite3FixExprList(pFix, pUp->pUpsertTarget)
       || sqlite3FixExpr(pFix, pUp->pUpsertTargetWhere)
       || sqlite3FixExprList(pFix, pUp->pUpsertSet)
       || sqlite3FixExpr(pFix, pUp->pUpsertWhere)
      ){
        return 1;
      }
    }
#endif
    pStep = pStep->pNext;
  }
  return 0;
}
#endif
