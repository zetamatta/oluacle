#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "oci.h"

#define TNAME_STATEMENT  "org.nyaos.oluacle.statement"
#define TNAME_CONNECTION "org.nyaos.oluacle.connection"
#define TNAME_ENVIRON    "org.nyaos.oluacle.environ"

#if 0
#  undef  DEBUG
#  define DEBUG(x) (x,fflush(stdout))
#else
#  define DEBUG(x) ;
#endif

#ifdef MEMORY_TEST
void *MALLOC(size_t size)
{
    void *p=(malloc)(size);
    fprintf(stderr,"ALLOC: %p\n",p);
    fflush(stderr);
    return p;
}
void FREE(void *p)
{
    fprintf(stderr,"FREE: %p\n",p);
    fflush(stderr);
    (free)(p);
}
#define malloc(x) MALLOC(x)
#define free(p) FREE(p)
#endif

static sword checkerr( lua_State *lua , OCIError *errhp , sword status )
{
    text errbuf[512];
    sb4 errcode;
    const char *funcname=NULL;

    if( status == OCI_SUCCESS )
        return status;

    luaL_where(lua,0);
    funcname=lua_tostring(lua,-1);

    switch (status){
    case OCI_SUCCESS_WITH_INFO:
        if( errhp != NULL ){
            OCIErrorGet ((dvoid *) errhp, (ub4) 1, (text *) NULL, &errcode,
                    errbuf, (ub4) sizeof(errbuf), (ub4) OCI_HTYPE_ERROR);
            return luaL_error(lua, "%sError - OCI_SUCCESS_WITH_INFO: %s" , funcname , errbuf );
        }else{
            return luaL_error(lua, "%sError - OCI_SUCCESS_WITH_INFO: ?" , funcname );
        }

    case OCI_NEED_DATA:
        return luaL_error(lua,"%sError - OCI_NEED_DATA",funcname);

    case OCI_NO_DATA:
        DEBUG( printf("%sError - OCI_NO_DATA\n",funcname) );
        break;

    case OCI_ERROR:
        if( errhp != NULL ){
            OCIErrorGet ((dvoid *) errhp, (ub4) 1, (text *) NULL, &errcode,
                    errbuf, (ub4) sizeof(errbuf), (ub4) OCI_HTYPE_ERROR);
            return luaL_error(lua,"%sError - %s", funcname,errbuf);
        }else{
            return luaL_error(lua,"%sError(%d)", funcname,status);
        }

    case OCI_INVALID_HANDLE:
        return luaL_error(lua,"%sError - OCI_INVALID_HANDLE",funcname);

    case OCI_STILL_EXECUTING:
        return luaL_error(lua,"%sError - OCI_STILL_EXECUTING",funcname);

    case OCI_CONTINUE:
        return luaL_error(lua,"%sError - OCI_CONTINUE",funcname);

    default:
        return luaL_error(lua,"%sError - %d", funcname,status);
    }
    return status;
}

static OCIEnv *olua_envhp(lua_State *lua)
{
    OCIEnv *envhp=NULL;
    sword status;

    lua_getfield(lua,LUA_REGISTRYINDEX,TNAME_ENVIRON);
    envhp = lua_touserdata(lua,-1);
    lua_pop(lua,1);
    if( envhp != NULL )
        return envhp;

    /* first */
    status = OCIEnvCreate(&envhp,OCI_DEFAULT,NULL,NULL,NULL,NULL,0,NULL);
    if( status != OCI_SUCCESS ){
        checkerr(lua,NULL,status);
        abort();
    }
    lua_pushlightuserdata(lua,envhp);
    lua_setfield(lua,LUA_REGISTRYINDEX,TNAME_ENVIRON);

    return envhp;
}

struct olua_bind_buffer {
    struct olua_bind_buffer *next;
    OCIBind *bind;
    union {
        char *s;
        unsigned char *u;
    }name;
    sb2 indicator;
    union{
        sword number;
        char  buffer[1];
    }u;
};

static struct olua_bind_buffer *olua_bind_buffer_new( size_t size )
{
    struct olua_bind_buffer *p = malloc( sizeof(struct olua_bind_buffer)+size );
    if( p == NULL )
        return NULL;

    p->next = NULL;
    p->bind = NULL;
    p->name.u = NULL;
    p->indicator = 0;
    p->u.buffer[0] = '\0';
    return p;
}

static void olua_bind_buffer_gc( struct olua_bind_buffer *p )
{
    while( p != NULL ){
        struct olua_bind_buffer *q=p->next;
        if( p->name.s ){
            free(p->name.s);
        }
        free(p);
        p=q;
    }
}

struct olua_fetch_buffer {
    ub2 type;
    ub2 size;
    ub2 len;
    sb2 ind;
    char *name;
    union{
        dvoid *pointor;
        char *string;
        int  *integer;
        double *number;
    }u;
    struct olua_fetch_buffer *next;
};

struct olua_fetch_buffer *olua_fetch_buffer_new(struct olua_fetch_buffer *self)
{
    if( self == NULL && (self=malloc(sizeof(struct olua_fetch_buffer)))==NULL )
        return NULL;
    
    self->size = 0;
    self->len = 0;
    self->ind = 0;
    self->name = NULL;
    self->u.pointor = NULL;
    self->next = NULL;

    return self;
}

static void olua_fetch_buffer_gc(struct olua_fetch_buffer *p)
{
    DEBUG( printf("ENTER: olua_fetch_buffer_gc(%p)\n",p ));
    while( p != NULL ){
        struct olua_fetch_buffer *q=p->next;
        if( p->u.pointor != NULL )
            free(p->u.pointor);
        if( p->name != NULL )
            free(p->name);
        free(p);
        p=q;
    }
    DEBUG( puts("LEAVE: olua_fetch_buffer_gc()"));
}

struct olua_statement {
    OCIStmt  *stmthp;
    OCIError *errhp;
    struct olua_bind_buffer  *bind_buffer;
    struct olua_fetch_buffer *fetch_buffer;
};

struct olua_statement *olua_statement_new(struct olua_statement *self)
{
    if( self == NULL && (self = malloc( sizeof(struct olua_statement) ))==NULL ){
        return NULL;
    }
    self->stmthp       = NULL;
    self->errhp        = NULL;
    self->bind_buffer  = NULL;
    self->fetch_buffer = NULL;
    return self;
}

/* lua-function: olua_statement_gc
 *  stack-in
 *    (+1) userdata-object for oci-handle
 *  return
 *    nothing
 */
static int olua_statement_gc(lua_State *lua)
{
    struct olua_statement *statement;
    
    if( lua_istable(lua,1) ){
        lua_getfield(lua,1,"handle");
        statement = lua_touserdata(lua,-1);
        lua_pop(lua,1);
    }else if( lua_isuserdata(lua,1) ){
        statement = lua_touserdata(lua,-1);
    }else{
        return luaL_error(lua,"olua_statement_gc: invalid statement-handle in parameter");
    }

    DEBUG( printf("ENTER: olua_statement_gc(%p)\n",statement) );
    /* statement-handle */
    if( statement->stmthp != NULL ){
        DEBUG( printf("OCIHandleFree(%p)\n",statement->stmthp) );
        OCIHandleFree( statement->stmthp , OCI_HTYPE_STMT );
        statement->stmthp = NULL;
    }
    /* error handle */
    if( statement->errhp != NULL ){
        OCIHandleFree( statement->errhp , OCI_HTYPE_ERROR );
        statement->errhp = NULL;
    }

    olua_bind_buffer_gc( statement->bind_buffer );
    statement->bind_buffer = NULL;
    olua_fetch_buffer_gc( statement->fetch_buffer );
    statement->fetch_buffer = NULL;
    DEBUG( puts("LEAVE: olua_statement_gc()") );
    return 0;
}

static void olua_checkhandle(lua_State *lua,const char *tname)
{
    const char *tname2="(null)";

    if( luaL_getmetafield(lua,-1,"__metatable")==0 ||
        strcmp(tname2=lua_tostring(lua,-1),tname) != 0 )
    {
        static char errormsg[256];

        sprintf(errormsg,"not %s(?%s)",tname,tname2);
        luaL_argerror(lua,1,errormsg);
    }
    lua_pop(lua,1);
}

static void *olua_tohandle(lua_State *lua,int index,const char *tname)
{
    void *userdata;
    if( lua_istable(lua,index) ){
        lua_getfield(lua,index,"handle");
        userdata=lua_touserdata(lua,-1);
        olua_checkhandle(lua,tname);
        lua_pop(lua,1);
    }else{
        userdata=lua_touserdata(lua,index);
        olua_checkhandle(lua,tname);
    }
    luaL_argcheck(lua,userdata != NULL,1,"not a oracle-handle");
    return userdata;
}

struct olua_connect {
    OCISvcCtx *svchp;
    OCIError  *errhp;
};

static int olua_disconnect(lua_State *lua)
{
    struct olua_connect *conn=olua_tohandle(lua,1,TNAME_CONNECTION);
    sword status;

    DEBUG( puts("olua_disconnect()") );
    
    if( conn != NULL && conn->svchp != NULL ){
        status = OCITransRollback(conn->svchp, conn->errhp, OCI_DEFAULT);
        if( status != OCI_SUCCESS )
            checkerr(lua,conn->errhp,status);
        status = OCILogoff( conn->svchp , conn->errhp );
        if( status != OCI_SUCCESS )
            checkerr(lua,conn->errhp,status);

        OCIHandleFree( conn->errhp , OCI_HTYPE_ERROR );
        conn->svchp = NULL;
        conn->errhp = NULL;
    }
    return 0;
}

static int olua_rollback(lua_State *lua)
{
    struct olua_connect *conn=olua_tohandle(lua,1,TNAME_CONNECTION);
    sword status;

    DEBUG( puts("ENTER olua_rollback()") );
    luaL_argcheck(lua,conn->svchp != NULL,1,"connection has beed closed.");
    status = OCITransRollback(conn->svchp, conn->errhp, OCI_DEFAULT);
    if( status != OCI_SUCCESS )
        checkerr(lua,conn->errhp,status);
    DEBUG( puts("LEAVE olua_rollback()") );
    return 0;
}

static int olua_commit(lua_State *lua)
{
    struct olua_connect *conn=olua_tohandle(lua,1,TNAME_CONNECTION);
    sword status;

    DEBUG( puts("ENTER olua_commit()") );
    luaL_argcheck(lua,conn->svchp != NULL,1,"connection has beed closed.");
    status = OCITransCommit(conn->svchp, conn->errhp, OCI_DEFAULT);
    if( status != OCI_SUCCESS )
        checkerr(lua,conn->errhp,status);
    DEBUG( puts("LEAVE olua_commit()") );
    return 0;
}

static int olua_exec(lua_State *lua);
static int olua_prepare( lua_State *lua );
static int olua_execute( lua_State *lua );
static int olua_bind( lua_State *lua );
static int olua_fetch( lua_State *lua );

int olua_connect( lua_State *lua )
{
    sword status;
    OCISvcCtx *svchp=NULL;
    OCIError  *errhp=NULL;
    const char *user   = luaL_checkstring(lua,1);
    const char *passwd = luaL_checkstring(lua,2);
    const char *dbname = NULL;
    int opt;
    OCIEnv *envhp=olua_envhp(lua);
    struct olua_connect *conn=NULL;

    if( lua_isstring(lua,3) ){
        dbname = lua_tostring(lua,3);
        opt=4;
    }else{
        dbname = "";
        opt=3;
    }

    DEBUG( printf("olua_connect(\"%s\",\"%s\",\"%s\")\n" 
                , user , passwd , dbname ) );

    putenv("NLS_DATE_FORMAT=YYYY/MM/DD HH24:MI:SS");

    /** error handle */
    status = OCIHandleAlloc(envhp , (dvoid**)&errhp , OCI_HTYPE_ERROR , 0 , NULL );
    if( status != OCI_SUCCESS ){
        checkerr(lua,NULL,status);
        abort();
    }

    /** login session */
    status = OCILogon( envhp , errhp , &svchp , 
              (CONST text *)user   , strlen(user) ,
              (CONST text *)passwd , strlen(passwd) ,
              (CONST text *)dbname , strlen(dbname) );

    if( status != OCI_SUCCESS ){
        checkerr(lua,errhp,status);
        abort();
    }

    if( svchp == NULL )
        return 0;
    
    /* create instance */
    if( lua_istable(lua,opt) ){
        lua_pushvalue(lua,opt);
    }else{
        lua_newtable(lua);
    }

    /* member: handle */
    if( (conn=lua_newuserdata(lua,sizeof(struct olua_connect))) == NULL){
        OCILogoff( svchp , errhp );
        return luaL_error(lua,"memory allocation error for userdata OCISvcCtx");
    }
    conn->svchp = svchp;
    conn->errhp = errhp; 

    /* meta-table */
    if( luaL_newmetatable(lua,TNAME_CONNECTION) ){
        lua_pushcfunction(lua,olua_disconnect);
        lua_setfield(lua,-2,"__gc");
        lua_pushstring(lua,TNAME_CONNECTION);
        lua_setfield(lua,-2,"__metatable");
    }
    lua_setmetatable(lua,-2);
    lua_setfield(lua,-2,"handle");

    /* method: exec */
    lua_pushcfunction(lua,olua_exec);
    lua_setfield(lua,-2,"exec");

    /* method: prepare */
    lua_pushcfunction(lua,olua_prepare);
    lua_setfield(lua,-2,"prepare");

    /* method: commit */
    lua_pushcfunction(lua,olua_commit);
    lua_setfield(lua,-2,"commit");

    /* method: rollback */
    lua_pushcfunction(lua,olua_rollback);
    lua_setfield(lua,-2,"rollback");

    /* method: disconnect */
    lua_pushcfunction(lua,olua_disconnect);
    lua_setfield(lua,-2,"disconnect");

    /* method: logoff */
    lua_pushcfunction(lua,olua_disconnect);
    lua_setfield(lua,-2,"logoff");

    /* method: close */
    lua_pushcfunction(lua,olua_disconnect);
    lua_setfield(lua,-2,"close");

    DEBUG( puts("successfully return 1") );
    return 1;
}


/** olua_prepare
 * stack-in:
 *   (+1) connection.
 *   (+2) sql string
 * stack-out (success)
 *   (+1) statement-handle
 */
static int olua_prepare( lua_State *lua )
{
    struct olua_statement *statement=NULL;
    sword status;
    ub4 prefetch = 0;
    OCIEnv *envhp = olua_envhp(lua);

    const char *sql = luaL_checkstring(lua,2);
    (void)olua_tohandle(lua,1,TNAME_CONNECTION);

    DEBUG( puts("ENTER: olua_prepare()"));

    /*** new statement object ***/
    lua_newtable(lua);
    statement = olua_statement_new( lua_newuserdata(lua,sizeof(struct olua_statement)));
    assert( statement != NULL );

    OCIHandleAlloc(envhp , (dvoid**)&statement->errhp , OCI_HTYPE_ERROR , 0 , NULL );

    if( luaL_newmetatable(lua,TNAME_STATEMENT) ){
        /* destructor */
        lua_pushcfunction(lua,olua_statement_gc); /* destructor's func */
        lua_setfield(lua,-2,"__gc"); /* assign destructor to meta table */

        lua_pushstring(lua,TNAME_STATEMENT);
        lua_setfield(lua,-2,"__metatable");
    }
    lua_setmetatable(lua,-2); /* assign metatable to user-object */
    lua_setfield(lua,-2,"handle");  /* assign user-object to instance */

    DEBUG( puts("CALL: OCIHandleAlloc") );
    status = OCIHandleAlloc(
        envhp ,
        (dvoid**)&statement->stmthp ,
        OCI_HTYPE_STMT  ,
        0 ,
        NULL );
    DEBUG( puts("EXIT: OCIHandleAlloc") );

    if( status != OCI_SUCCESS ){
        checkerr(lua,statement->errhp,status);
        abort();
    }

    status = OCIAttrSet(statement->stmthp, OCI_HTYPE_STMT,
		(dvoid *)&prefetch, (ub4)0, (ub4)OCI_ATTR_PREFETCH_ROWS,
		statement->errhp);

    if( status != OCI_SUCCESS ){
        checkerr(lua,statement->errhp,status);
        abort();
    }

    DEBUG( printf("SQL=[%s]\n",sql) );
    status = OCIStmtPrepare(
        statement->stmthp , statement->errhp, (CONST text*)sql , (ub4)strlen(sql) ,
        (ub4)OCI_NTV_SYNTAX , (ub4)OCI_DEFAULT );

    DEBUG( printf("OCIStmtPrepare()=%d\n",status));

    if( status != OCI_SUCCESS ){
        checkerr(lua,statement->errhp,status);
        abort();
    }
    DEBUG( printf("Statement-handle=%p\n",statement->stmthp) );
    DEBUG( puts("LEAVE: olua_prepare(success)"));

    /* stack:
     *   1:connection 
     *   2:sql
     * ------
     *   3:object
     *   4:meta-table (DEL)
     *   5:key (DEL)
     *   6:value (DEL)
     */

    /* method: bind */
    lua_pushcfunction(lua,olua_bind);
    lua_setfield(lua,-2,"bind");

    /* method: execute */
    lua_pushcfunction(lua,olua_execute);
    lua_setfield(lua,-2,"execute");

    /* method: fetch */
    lua_pushcfunction(lua,olua_fetch);
    lua_setfield(lua,-2,"fetch");

    /* member: connection */
    lua_pushvalue(lua,1);
    lua_setfield(lua,-2,"connection");

    /* member: sql-string */
    lua_pushvalue(lua,2);
    lua_setfield(lua,-2,"sql");

    return 1;
}

/*
 * -nbinds-1   : statement-handle
 * -nbinds..-1 : bind-variables
 */
static int olua_bind_core( lua_State *lua , int nbinds )
{
    int i;
    struct olua_statement *statement=olua_tohandle(lua,-nbinds-1,TNAME_STATEMENT);
    sword status;

    for(i=0;i<nbinds;i++){
        int sp=-nbinds+i;
        struct olua_bind_buffer *b;

        DEBUG( printf("try bind %d\n",i+1));
        DEBUG( printf("Statement-handle=%p\n",statement->stmthp) );


        if( lua_istable(lua,sp) ){
            lua_pushnil(lua);
            while( lua_next(lua,sp-1) ){
                const char *key; size_t key_len; char *key2;

                lua_pushvalue(lua,-2);
                key = lua_tolstring(lua,-1,&key_len);

                if( key[0] == ':' ){
                    key2 = strdup(key);
                }else{
                    key2 = malloc(++key_len+2);
                    key2[0] = ':';
                    strcpy(key2+1,key);
                }

                if( lua_toboolean(lua,-2) == 0 ){
                    b=olua_bind_buffer_new(0);
                    b->name.s = key2;
                    b->u.buffer[0] = '\0';
                    b->indicator = OCI_IND_NULL ;

                    DEBUG( printf("BIND: %s=>NULL\n" , key2) );

                    status = OCIBindByName( 
                                statement->stmthp ,
                                &b->bind ,
                                statement->errhp ,
                                b->name.u ,
                                key_len ,
                                (dvoid *)b->u.buffer , /* valuep */
                                1 , /* value_sz */
                                SQLT_STR , /* dty */ 
                                &b->indicator ,
                                NULL ,
                                NULL ,
                                0 ,
                                NULL ,
                                OCI_DEFAULT );


                }else if( lua_isnumber(lua,-2) ){
                    b=olua_bind_buffer_new(0);
                    b->name.s = key2;
                    b->u.number = lua_tonumber(lua,-2);

                    DEBUG( printf("BIND: %s[len=%d]=>%d(number)\n" ,
                                    key2,key_len,b->u.number) );

                    status = OCIBindByName( 
                                statement->stmthp ,
                                &b->bind ,
                                statement->errhp ,
                                b->name.u ,
                                -1 ,
                                (dvoid *)&b->u.number , /* valuep */
                                (sword)sizeof(sword) , /* value_sz */
                                SQLT_INT , /* dty */ 
                                &b->indicator ,
                                NULL ,
                                NULL ,
                                0 ,
                                NULL ,
                                OCI_DEFAULT );
                }else{
                    size_t val_len; 

                    b=olua_bind_buffer_new(val_len + 1);
                    b->name.s = key2;
                    strcpy( b->u.buffer , lua_tolstring(lua,-2,&val_len) );

                    DEBUG( printf("BIND: %s=>%s(string)\n" , key2,b->u.buffer) );

                    status = OCIBindByName( 
                                statement->stmthp ,
                                &b->bind ,
                                statement->errhp ,
                                b->name.u ,
                                key_len ,
                                b->u.buffer ,
                                val_len+1 ,
                                SQLT_STR , 
                                &b->indicator ,
                                NULL ,
                                NULL ,
                                0 ,
                                NULL ,
                                OCI_DEFAULT );
                }
                if( status != OCI_SUCCESS ){
                    free( b );
                    checkerr(lua,statement->errhp,status);
                    abort();
                }
                b->next = statement->bind_buffer ;
                statement->bind_buffer = b;

                lua_pop(lua,2); /* drop value and key duplicated */
            }
            continue;
        }else if( lua_toboolean(lua,sp)==0 ){ /* nil or false => NULL */
            b=olua_bind_buffer_new(0);
            b->indicator = OCI_IND_NULL ;
            status = OCIBindByPos( statement->stmthp , 
                          &b->bind ,
                          statement->errhp ,
                          i+1 , 
                          (dvoid*)b->u.buffer , /* valuep */
                          1 , /* value_sz */
                          SQLT_STR , /* dty */
                          &b->indicator ,
                          NULL ,
                          NULL ,
                          0 ,
                          NULL ,
                          OCI_DEFAULT 
                    );

        }else if( lua_isnumber(lua,sp) ){
            b=olua_bind_buffer_new(0);
            assert( b != NULL );

            b->u.number = lua_tointeger(lua,sp);

            DEBUG( printf("find %d(as number)\n",b->u.number) );

            status = OCIBindByPos( statement->stmthp , 
                        &b->bind ,
                        statement->errhp ,
                        i+1 , /* position */
                        (dvoid *)&b->u.number , /* valuep */
                        (sword)sizeof(sword) ,  /* value_sz */
                        SQLT_INT , /* dty */
                        &b->indicator ,
                        (ub2*)NULL ,
                        (ub2*)NULL , 
                        (ub4)0 ,  /* maxarr_len */
                        (ub4*)NULL , /* curelep */
                        OCI_DEFAULT /* mode */
                    ); 
        }else{
            const char *string=lua_tostring(lua,sp);
            size_t len;
            
            assert( string != NULL );
            len=strlen(string);

            b=olua_bind_buffer_new(len);
            assert( b != NULL );
            strcpy( b->u.buffer , string );

            DEBUG( printf("find '%s' (as string)\n",string) );
            status = OCIBindByPos( statement->stmthp , 
                          &b->bind ,
                          statement->errhp ,
                          i+1 , 
                          (dvoid*)b->u.buffer , /* valuep */
                          len+1 , /* value_sz */
                          SQLT_STR , /* dty */
                          &b->indicator ,
                          NULL ,
                          NULL ,
                          0 ,
                          NULL ,
                          OCI_DEFAULT 
                    );
        }
        if( status != OCI_SUCCESS ){
            free( b );
            checkerr(lua,statement->errhp,status);
            abort();
        }
        b->next = statement->bind_buffer ;
        statement->bind_buffer = b;
    }
    lua_pushboolean(lua,1);
    DEBUG( printf("LEAVE: olua_bind(successfully)\n") );
    return 1;
}

/** olua_bind
 *
 * stack-in:
 *  +1   = statement-object(table)
 *  +2.. = bind-variables
 * stack-out:
 *  +3   = true:succcess , nil:failure
 */
static int olua_bind( lua_State *lua )
{
    int i=lua_gettop(lua);
    return olua_bind_core(lua,i-1);
}

/* olua_fetch_buffer_alloc */
static struct olua_fetch_buffer *olua_fetch_buffer_alloc(
    lua_State *lua ,
    struct olua_statement *statement )
{
    sb4 parm_status;
    dvoid *mypard;
    ub4 counter=0;

    struct olua_fetch_buffer dummyfirst;
    struct olua_fetch_buffer *curr=&dummyfirst;

    DEBUG( puts("ENTER olua_fetch_buffer_alloc()") );

    olua_fetch_buffer_new( &dummyfirst );

    while( (parm_status=OCIParamGet(
                statement->stmthp ,
                OCI_HTYPE_STMT ,
                statement->errhp ,
                &mypard ,
                ++counter )) == OCI_SUCCESS )
    {
        sword status;
        const char *colname;
        ub4 colname_len;

        if( (curr->next = olua_fetch_buffer_new(NULL)) == NULL ){
            olua_fetch_buffer_gc( dummyfirst.next );
            luaL_error(lua,"memory allocation error(1)");
            return NULL;
        }
        curr = curr->next;

        /* データサイズ取得 */
        status = OCIAttrGet(
            mypard ,
            (ub4)OCI_DTYPE_PARAM ,
            (dvoid*)&curr->size ,
            (ub4)0 ,
            (ub4)OCI_ATTR_DATA_SIZE , 
            statement->errhp
        );
        if( status != OCI_SUCCESS ){
            DEBUG( puts("can not get datasize") );
            olua_fetch_buffer_gc(dummyfirst.next);
            checkerr(lua,statement->errhp,status);
            return NULL;
        }
        DEBUG( printf("DATASIZE=%d\n",curr->size) );

        /* データ属性取得 */
        status = OCIAttrGet(
            (dvoid*)mypard , (ub4)OCI_DTYPE_PARAM ,
            (dvoid*)&curr->type , 
            (ub4)0 ,
            (ub4)OCI_ATTR_DATA_TYPE , 
            statement->errhp
        );
        if( status != OCI_SUCCESS ){
            olua_fetch_buffer_gc(dummyfirst.next);
            checkerr(lua,statement->errhp,status);
            return NULL;
        }
        DEBUG( printf("DATATYPE=%d\n" , curr->type ) );

        if( curr->type == SQLT_NUM ){
            curr->type = SQLT_FLT;
            curr->size = sizeof(double);
        }else if( curr->type == SQLT_DAT ){
            curr->type = SQLT_STR;
            curr->size = 32;
        }
        /* 領域確保 */
        if( (curr->u.pointor = malloc(curr->size+1))==NULL ){
            olua_fetch_buffer_gc( dummyfirst.next );
            luaL_error(lua,"olua_fetch_buffer_alloc(): memory allocation error");
            return NULL;
        }

        /* 列名取得 */
        status = OCIAttrGet(
            (dvoid*)mypard, (ub4)OCI_DTYPE_PARAM ,
            (dvoid*)&colname ,
            &colname_len ,
            (ub4)OCI_ATTR_NAME ,
            statement->errhp 
        );
        if( status != OCI_SUCCESS ){
            olua_fetch_buffer_gc(dummyfirst.next);
            checkerr(lua,statement->errhp,status);
            abort();
        }
        if( (curr->name = malloc( colname_len+1 )) == NULL ){
            olua_fetch_buffer_gc( dummyfirst.next );
            luaL_error(lua,"olua_fetch_buffer_alloc(): memory allocation error");
            return NULL;
        }
        memcpy( curr->name , colname , colname_len ); 
        curr->name[ colname_len ] = '\0';
        DEBUG( printf("COLUMN=[%.*s](%d)\n" , colname_len , colname , colname_len ));
    }
    DEBUG(puts("ENTER: OCIDefineByPos"));

    counter=0;
    for( curr=dummyfirst.next ; curr != NULL ; curr=curr->next ){
        OCIDefine *dfp=NULL;
        sword status;

        status = OCIDefineByPos(
            statement->stmthp ,
            &dfp ,
            statement->errhp ,
            ++counter ,
            curr->u.pointor ,
            curr->size ,
            curr->type ,
            &curr->ind  ,
            &curr->len ,
            (ub4)NULL,
            OCI_DEFAULT
        );
        if( status != OCI_SUCCESS ){
            olua_fetch_buffer_gc( dummyfirst.next );
            checkerr( lua , statement->errhp , status );
            return NULL;
        }
    }
    if( statement->fetch_buffer != NULL ){
        olua_fetch_buffer_gc( statement->fetch_buffer );
        statement->fetch_buffer = NULL;
    }
    
    DEBUG( printf("LEAVE olua_fetch_buffer_alloc(%p)\n",dummyfirst.next) );
    return statement->fetch_buffer = dummyfirst.next;
}

/** olua_execute
 * stack-in:
 *   (-1) statement-handle
 * stack-out:
 *   nothing
 */
static int olua_execute(lua_State *lua)
{
    struct olua_statement *statement = olua_tohandle(lua,-1,TNAME_STATEMENT);
    OCISvcCtx **conn;
    sword status;
    ub2 type;
    ub4 iters;

    DEBUG( puts("ENTER: olua_execute()") );

    /* get connection */
    lua_getfield(lua,-1,"connection");
    conn = olua_tohandle(lua,-1,TNAME_CONNECTION);
    lua_pop(lua,1);
    
    if( statement->stmthp == NULL)
        return luaL_error(lua,"olua_execute: stmt handle is nil.");

    status = OCIAttrGet(statement->stmthp, (ub4) OCI_HTYPE_STMT,
		(dvoid *)&type, (ub4 *)0, (ub4)OCI_ATTR_STMT_TYPE, statement->errhp);
    if( status != OCI_SUCCESS )
        return checkerr(lua,statement->errhp,status);
    
    if (type == OCI_STMT_SELECT)
        iters = 0;
    else
        iters = 1;

    DEBUG( puts("call OCIStmtExecute()") );
    status = OCIStmtExecute(*conn,statement->stmthp,statement->errhp,iters,0,NULL,NULL,OCI_DEFAULT);
    if( status != OCI_SUCCESS )
        return checkerr(lua,statement->errhp,status);
    
    if( type == OCI_STMT_SELECT ){
        olua_fetch_buffer_alloc(lua,statement);
        lua_pushcfunction(lua,olua_fetch);
        lua_insert(lua,-2);
        DEBUG( puts("LEAVE: olua_execute(OCI_STMT_SELECT)") );
        return 2;
    }else{
        ub4 rowcount;
        status = OCIAttrGet(statement->stmthp, (ub4) OCI_HTYPE_STMT,
                    (dvoid *)&rowcount, (ub4 *)0, (ub4)OCI_ATTR_ROW_COUNT, statement->errhp);
        if( status != OCI_SUCCESS )
            return checkerr(lua,statement->errhp,status);
        
        DEBUG( puts("LEAVE: olua_execute(! OCI_STMT_SELECT)") );
        lua_pushinteger(lua,rowcount);
        return 1;
    }
}

/** olua_fetch 
 *
 * stack-in:
 *   (+1) statement-handle
 * stack-out:
 *   (+1) column table.
 */
static int olua_fetch(lua_State *lua)
{
    struct olua_statement *statement=olua_tohandle(lua,1,TNAME_STATEMENT);
    struct olua_fetch_buffer *fetch_buffer=NULL;

    sword status;
    int counter=0;

    DEBUG( puts("ENTER: olua_fetch()") );

    if( statement == NULL )
        return luaL_error(lua,"error: invalid parameter(statement==NULL)");

    fetch_buffer = statement->fetch_buffer ;

    status = OCIStmtFetch2(
            statement->stmthp ,
            statement->errhp ,
            (ub4)1 ,
            OCI_FETCH_NEXT ,
            0 ,
            OCI_DEFAULT );

    if( status == OCI_NO_DATA){
        statement=lua_touserdata(lua,-1);
        olua_statement_gc(lua);
        lua_pushnil(lua);
        return 1;
    }
    if( status != OCI_SUCCESS )
        return checkerr(lua,statement->errhp,status);
    
    DEBUG( puts("push fetch values") );
    DEBUG( fflush(stdout) );
    
    lua_newtable(lua);
    for( counter=1 ; fetch_buffer != NULL ; ++counter ){
        lua_pushstring(lua,fetch_buffer->name);
        lua_pushinteger(lua,counter);
        if( fetch_buffer->ind != 0 ){ /* NULL VALUE */
            lua_getfield(lua,1,"connection");
            lua_getfield(lua,-1,"null");
            if( lua_isnil(lua,-1) ){
                lua_pop(lua,1);
                lua_pushboolean(lua,0);
            }
            lua_remove(lua,-2);
            DEBUG(puts("ind==0"));
        }else{ /* NOT NULL */
            switch( fetch_buffer->type ){
            case SQLT_STR:
            case SQLT_CHR:
            case SQLT_VCS:
            case SQLT_AFC:
                lua_pushlstring(lua,fetch_buffer->u.string,fetch_buffer->len);
                break;
            case SQLT_INT:
                lua_pushinteger(lua,*fetch_buffer->u.integer);
                break;
            /* case SQLT_BDOUBLE: */
            /* case SQLT_BFLOAT: */
            case SQLT_FLT:
                lua_pushnumber(lua,*fetch_buffer->u.number);
                break;
            case SQLT_ODT:
            case SQLT_DATE:
            case SQLT_TIMESTAMP:
            case SQLT_TIMESTAMP_TZ:
            case SQLT_TIMESTAMP_LTZ:
            default:
                lua_pushnil(lua);
                break;
            }
        }
        lua_pushvalue(lua,-1);
        lua_insert(lua,-3);
        /* 1:table
         * 2:key-string
         * 3:value
         * 4:counter
         * 5:value
         */
        lua_settable(lua,-5);
        lua_settable(lua,-3);
        fetch_buffer = fetch_buffer->next;
    }
    DEBUG( puts("LEAVE: olua_fetch(successfully)") );
    return 1;
}


/** olua_exec
 *
 * stack-in:
 *   (+1) connection.
 *   (+2) sql string
 *   (+3) bind values
 * stack-out
 *   (+1) iterator(fetch-function)
 *   (+2) statement-handle
 */
static int olua_exec(lua_State *lua)
{
    int bindvars=lua_gettop(lua)-2;

    /* +1 connection
     * +2 sql
     * +3... binds
     */
    DEBUG( printf("stack=%d (before prepare)\n",lua_gettop(lua) ) );

    olua_prepare(lua);

    /* +1 connection  => DEL
     * +2 sql string  => DEL
     *  : bind-values
     * -1 statement-object
     */
    assert( lua_gettop(lua) == bindvars + 3 );

    lua_remove(lua,+2); /* del: sql(original) */
    lua_remove(lua,+1); /* del: connection(original) */
    lua_insert(lua,+1); /* mov: statement-handle */

    DEBUG( printf("stack=%d (before bind)\n",lua_gettop(lua) ) );

    /* +1 statement-handle
     *  : binds
     */
    assert( lua_gettop(lua) == bindvars + 1 );
    olua_bind_core(lua,bindvars);
    if( lua_isnil(lua,-1) )
        return 1;
    lua_settop(lua,1);

    /* +1 statement-handle */
    assert( lua_gettop(lua) == 1 );

    return olua_execute(lua);
}

int luaopen_oluacle(lua_State *lua)
{
    lua_newtable(lua);
    lua_pushcfunction(lua,olua_connect);
    lua_setfield(lua,-2,"new");
    return 1;
}
