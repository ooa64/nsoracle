/*
 *  An Oracle OCI internal driver for AOLserver
 *
 *  Copyright (C) 1997 Cotton Seed
 *
 *  Documented 1998    by cottons@concmp.com, philg@mit.edu, shivers@lcs.mit.edu
 *  Extended 1999      by markd@arsdigita.com
 *  Extended 2000      by markd@arsdigita.com, curtisg@arsdigita.com, jsalz@mit.edu,
 *                        jsc@arsdigita.com, mayoff@arsdigita.com
 *  Extended 2002-2004 by Jeremy Collins <jeremy.collins@tclsource.org>
 *  Extended 2014-2024 by Gustaf Neumann (cleanup, NaviServer adjustments)
 */

#include "nsoracle.h"

static sb2 null_ind = -1;
static ub2 rc = 0;
static ub4 rl = 0;
static bool convert_encoding_p = NS_FALSE;

NS_EXPORT NsDb_DriverInitProc Ns_DbDriverInit;

/*
 * [ns_ora] implementation.
 */

/*{{{ Ns_OracleMalloc */
static dvoid *Ns_OracleMalloc(dvoid *UNUSED(cxt), size_t size) {
    return Ns_Malloc(size);
}
/*}}}*/

/*{{{ Ns_OracleRealloc */
static dvoid *Ns_OracleRealloc(dvoid *UNUSED(cxt), dvoid *buf, size_t size) {
    return Ns_Realloc(buf, size);
}
/*}}}*/

/*{{{ Ns_OracleFree */
static void Ns_OracleFree(dvoid *UNUSED(cxt), dvoid *buf) {
    Ns_Free(buf);
}
/*}}}*/

/*{{{ DynamicBindIn
 *----------------------------------------------------------------------
 * DynamicBindIn --
 *
 *      Used to dynamically set IN parameters OraclePLSQLObjCmd.
 *
 *----------------------------------------------------------------------
 */
static sb4
DynamicBindIn(dvoid * ictxp,
              OCIBind * UNUSED(bindp),
              ub4 UNUSED(iter),
              ub4 UNUSED(index),
              dvoid ** bufpp,
              ub4 * alenp, ub1 * piecep, dvoid ** indpp)
{
    fetch_buffer_t   *fbPtr = (fetch_buffer_t *) ictxp;
    ora_connection_t *connection = fbPtr->connection;
    const char     *value = NULL;

    if(fbPtr->name != NULL) {
        value = Tcl_GetVar(connection->interp, fbPtr->name, 0);
    } else if (fbPtr->buf != NULL) {
        value = fbPtr->buf;
    }

    *bufpp = (void *)value;
    *alenp = (ub4)strlen(value) + 1;
    *piecep = OCI_ONE_PIECE;
    *indpp = NULL;

    fbPtr->inout = BIND_IN;

    return OCI_CONTINUE;
}
/*}}}*/

/*{{{ DynamicBindOut
 *----------------------------------------------------------------------
 * DynamicBindOut --
 *
 *      Used to dynamically allocate more memory for IN/OUT and
 *      OUT parameters in OraclePLSQLObjCmd.
 *
 *----------------------------------------------------------------------
 */
static sb4
DynamicBindOut(dvoid * ctxp, OCIBind * UNUSED(bindp),
               ub4 iter, ub4 index, dvoid ** bufpp,
               ub4 ** alenpp, ub1 * piecep,
               dvoid ** indpp, ub2 ** rcodepp)
{
    fetch_buffer_t   *fetchbuf = (fetch_buffer_t *) ctxp;

    ns_ora_log(lexpos(), "entry (dbh %p; iter %d, index %d)", ctxp, iter, index);

    if (iter != 0) {
        error(lexpos(), "iter != 0");
        return NS_ERROR;
    }

    if (*piecep == OCI_ONE_PIECE || *piecep == OCI_FIRST_PIECE) {
        fetchbuf->fetch_length = 0;
    } else if (*piecep == OCI_NEXT_PIECE) {
        fetchbuf->fetch_length += fetchbuf->piecewise_fetch_length;
    }

    if (fetchbuf->fetch_length >= fetchbuf->buf_size / 2) {
        fetchbuf->buf_size += EXEC_PLSQL_BUFFER_SIZE;
        fetchbuf->buf = Ns_Realloc (fetchbuf->buf, fetchbuf->buf_size);
    }

    fetchbuf->piecewise_fetch_length = fetchbuf->buf_size - fetchbuf->fetch_length;

    ns_ora_log(lexpos(), "%d, %d, %d",
        fetchbuf->buf_size,
        fetchbuf->fetch_length,
        fetchbuf->piecewise_fetch_length);

    *bufpp = &fetchbuf->buf[fetchbuf->fetch_length];
    *alenpp = &fetchbuf->piecewise_fetch_length;
    *indpp = &fetchbuf->is_null;
    *rcodepp = &rc;

    fetchbuf->inout = BIND_OUT;

    return OCI_CONTINUE;
}
/*}}}*/

/*{{{ OracleObjCmd
 *----------------------------------------------------------------------
 * OracleObjCmd --
 *
 *      Implements [ns_ora] command.
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
int
OracleObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    Ns_DbHandle *dbh;

    static const char *subcmds[] = {
        "plsql", "exec_plsql", "exec_plsql_bind", "desc", "select",
        "dml", "array_dml", "1row", "0or1row", "getcols", "resultrows",
        "clob_get_file", "blob_get_file",
        "clob_dml_bind", "clob_dml_file_bind",
        "blob_dml_bind", "blob_dml_file_bind",
        "clob_dml", "clob_dml_file",
        "blob_dml", "blob_dml_file",
        "write_clob", "write_blob",
        NULL
    };

    enum ISubCmdIdx {
        CPLSQL, CExecPLSQL, CExecPLSQLBind, CDesc, CSelect,
        CDML, CArrayDML, C1Row, C0or1Row, CGetCols, CResultRows,
        CClobGetFile, CBlobGetFile,
        CClobDMLBind, CClobDMLFileBind,
        CBlobDMLBind, CBlobDMLFileBind,
        CClobDML, CClobDMLFile,
        CBlobDML, CBlobDMLFile,
        CWriteClob, CWriteBlob
    } subcmd;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "subcommand ?args?");
        return TCL_ERROR;
    }

    if (Tcl_GetIndexFromObj(interp, objv[1], subcmds, "command", TCL_EXACT,
            (int *)&subcmd) != TCL_OK) {
        return TCL_ERROR;
    }

    if (Ns_TclDbGetHandle(interp, Tcl_GetString(objv[2]), &dbh) != TCL_OK) {
        return TCL_ERROR;
    }

    if (Ns_DbDriverName(dbh) != ora_driver_name) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp), "handle: '",
                Tcl_GetString(objv[1]), "' is not of type ",
                               ora_driver_name, (char*)0L);
        return TCL_ERROR;
    }

    if (dbh->connection == NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("error: no connection", TCL_INDEX_NONE));
        return TCL_ERROR;
    }

    switch (subcmd) {
        case CPLSQL:

            Ns_OracleFlush(dbh);
            return OraclePLSQL(interp, objc, objv, dbh);

        case CExecPLSQL:

            Ns_OracleFlush(dbh);
            return OracleExecPLSQL(interp, objc, objv, dbh);

        case CExecPLSQLBind:

            Ns_OracleFlush(dbh);
            return OracleExecPLSQLBind(interp, objc, objv, dbh);

        case CDesc:

            Ns_OracleFlush(dbh);
            return OracleDesc(interp, objc, objv, dbh);

        case CDML:
        case CArrayDML:
        case CSelect:
        case C1Row:
        case C0or1Row:

            Ns_OracleFlush(dbh);
            return OracleSelect(interp, objc, objv, dbh);

        case CGetCols:

            Ns_OracleFlush(dbh);
            return OracleGetCols(interp, objc, objv, dbh);

        case CResultRows:

            return OracleResultRows(interp, objc, objv, dbh);

        case CClobDML:
        case CClobDMLFile:
        case CBlobDML:
        case CBlobDMLFile:

            Ns_OracleFlush(dbh);
            return OracleLobDML(interp, objc, objv, dbh);

        case CClobDMLBind:
        case CClobDMLFileBind:
        case CBlobDMLBind:
        case CBlobDMLFileBind:

            Ns_OracleFlush(dbh);
            return OracleLobDMLBind(interp, objc, objv, dbh);

        case CClobGetFile:
        case CBlobGetFile:
        case CWriteClob:
        case CWriteBlob:

            Ns_OracleFlush(dbh);
            return OracleLobSelect(interp, objc, objv, dbh);

        default:

            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                    "unknown command \"", Tcl_GetString(objv[1]),
                    "\": should be "
                    "dml, array_dml, "
                    "resultid, resultrows, clob_dml, clob_dml_file, "
                    "clob_get_file, blob_dml, blob_dml_file, "
                    "blob_get_file, dml, select, 1row, "
                     "0or1row, or exec_plsql.", (char*)0L);
    }

    return NS_OK;
}
/*}}}*/

/*{{{ OraclePLSQL
 *----------------------------------------------------------------------
 * OraclePLSQL --
 *
 *      Implements [ns_ora plsql] command.
 *
 *      ns_oracle plsql dbhandle sql ?ref?
 *
 * Results:
 *
 *      Nothing.
 *
 * Side effects:
 *
 *      Tcl variables may be set if IN/OUT, or OUT variables exist
 *      in the PL/SQL call.
 *
 *----------------------------------------------------------------------
 */
int
OraclePLSQL(Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, Ns_DbHandle *dbh)
{
    ora_connection_t  *connection;
    oci_status_t       oci_status;
    string_list_elt_t *bind_variables,
                      *var_p;
    char              *query;
    const char        *ref;
    int                i, refcursor_count = 0;

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "dbhandle sql ?ref?");
        return TCL_ERROR;
    }

    connection = dbh->connection;
    connection->interp = interp;
    query = Tcl_GetString(objv[3]);

    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & connection->stmt,
                                OCI_HTYPE_STMT, 0, NULL);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIHandleAlloc", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    oci_status = OCIStmtPrepare(connection->stmt,
                                connection->err,
                                (const OraText *)query,
                                (ub4) strlen(query),
                                OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtPrepare", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    if (objc == 5) {
        ref = Tcl_GetString(objv[4]);
    } else {
        ref = "";
    }

    bind_variables = parse_bind_variables(query);
    connection->n_columns = string_list_len(bind_variables);
    malloc_fetch_buffers(connection);

    /*
     * Loop through bind variables and allocate memory for
     * IN/OUT, and OUT variables, then bind them to the
     * statement.
     *
     */
    for (var_p = bind_variables, i = 0; var_p != NULL;
         var_p = var_p->next, i++) {

        fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];
        const char *value;

        fetchbuf->type = (OCITypeCode)-1;

        value = Tcl_GetVar(interp, var_p->string, 0);
        fetchbuf->name = var_p->string;

        if ((value == NULL)
            && (strcmp(var_p->string, ref) != 0)
            ) {
            /* The only time a bind variable can not exist is if its strictly
               an OUT variable, or if its a REF CURSOR.  */
            Tcl_AppendResult(interp, " bind variable :", var_p->string,
                    " does not exist. ", (char*)0L);
            Ns_OracleFlush(dbh);
            string_list_free_list(bind_variables);
            free_fetch_buffers(connection);
            return TCL_ERROR;
        } else if ( strcmp(var_p->string, ref) == 0 ) {
            /* Handle REF CURSOR */

            if (refcursor_count == 1) {
                Tcl_SetObjResult(interp, Tcl_NewStringObj("invalid plsql statement, you"
                        " can only have a single ref cursors. ", TCL_INDEX_NONE));
                return TCL_ERROR;
            } else {
                refcursor_count = 1;
            }

            fetchbuf->external_type = SQLT_RSET;
            fetchbuf->inout         = BIND_OUT;
            fetchbuf->size          = 0;

            oci_status = OCIHandleAlloc (connection->env,
                                         (oci_handle_t **) &fetchbuf->stmt,
                                         OCI_HTYPE_STMT,
                                         0, NULL);
            if (oci_error_p(lexpos(), dbh, "OCIHandleAlloc", query, oci_status)) {
                Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                              TCL_VOLATILE);
                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                free_fetch_buffers(connection);
                return TCL_ERROR;
            }

            oci_status = OCIBindByName(connection->stmt,
                                       &fetchbuf->bind,
                                       connection->err,
                                       (const OraText *)var_p->string,
                                       (sb4) strlen(var_p->string),
                                       &fetchbuf->stmt,
                                       0,
                                       fetchbuf->external_type,
                                       &fetchbuf->is_null,
                                       0, 0, 0, 0, OCI_DEFAULT);

            if (oci_error_p(lexpos(), dbh, "OCIBindByName", query, oci_status)) {
                Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                              TCL_VOLATILE);
                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                free_fetch_buffers(connection);
                return TCL_ERROR;
            }

            if (oci_error_p(lexpos(), dbh, "OCIBindByName", query, oci_status)) {
                Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                              TCL_VOLATILE);
                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                free_fetch_buffers(connection);
                return TCL_ERROR;
            }

        } else {
            /* Handle everything else.  If we get this far then
             * we don't have a REF CURSOR at this bind location so we
             * just bind it as a SQLT_STR instead.
             *
             * This breaks overloading because Oracle cannot determine
             * what type is being sent.
             */

            fetchbuf->external_type = SQLT_STR;
            fetchbuf->is_null = 0;

            oci_status = OCIBindByName(connection->stmt,
                                       &fetchbuf->bind,
                                       connection->err,
                                       (const OraText *)var_p->string,
                                       (sb4) strlen(var_p->string),

                                       NULL,                     /* valuep */
                                       MAX_DYNAMIC_BUFFER,       /* value_sz */
                                       fetchbuf->external_type,  /* dty */
                                       &fetchbuf->is_null,       /* indp */
                                       0,                        /* alenp */
                                       0, 0, 0,
                                       OCI_DATA_AT_EXEC);

            if (oci_error_p(lexpos(), dbh, "OCIBindByName", query, oci_status)) {
                Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                              TCL_VOLATILE);
                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                free_fetch_buffers(connection);
                return TCL_ERROR;
            }

            oci_status = OCIBindDynamic(fetchbuf->bind,
                                        connection->err,
                                        fetchbuf, DynamicBindIn,
                                        fetchbuf, DynamicBindOut);

            if (oci_error_p(lexpos(), dbh, "OCIBindByName", query, oci_status)) {
                Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                              TCL_VOLATILE);
                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                free_fetch_buffers(connection);
                return TCL_ERROR;
            }


        }

    }

    oci_status = OCIStmtExecute(connection->svc,
                                connection->stmt,
                                connection->err,
                                1,
                                0, NULL, NULL,
                                (connection->mode == autocommit
                                 ? OCI_COMMIT_ON_SUCCESS :
                                 OCI_DEFAULT));

    if (oci_error_p(lexpos(), dbh, "OCIStmtExecute", query, oci_status)) {
        Ns_OracleFlush(dbh);
        free_fetch_buffers(connection);
        return TCL_ERROR;
    }

    /*
     * Loop through bind variables again this time pulling out the
     * new value from OUT variables.
     *
     */
    for (var_p = bind_variables, i = 0; var_p != NULL;
         var_p = var_p->next, i++) {
        fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];

        if (fetchbuf->inout == BIND_OUT) {
            switch (fetchbuf->external_type) {

                case SQLT_STR:
                    Tcl_SetVar(interp, var_p->string, fetchbuf->buf, 0);
                    break;

                case SQLT_RSET:

                    oci_status = OCIHandleFree (connection->stmt,
                                                OCI_HTYPE_STMT);
                    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtExecute", query, oci_status)) {
                        Ns_OracleFlush(dbh);
                        free_fetch_buffers(connection);
                        return TCL_ERROR;
                    }

                    connection->stmt = fetchbuf->stmt;
                    break;
            }
        }
    }

    string_list_free_list(bind_variables);
    free_fetch_buffers(connection);

    return NS_OK;
}
/*}}}*/

/*{{{ OracleExecPLSQL
 *----------------------------------------------------------------------
 * OracleExecPLSQL --
 *
 *      Implements [ns_ora exec_plsql]
 *
 *      ns_ora exec_plsql dbhandle sql
 *
 * Results:
 *
 *      Nothing.
 *
 *----------------------------------------------------------------------
 */
int
OracleExecPLSQL(Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, Ns_DbHandle *dbh)
{
    OCIBind           *bind;
    ora_connection_t  *connection;
    oci_status_t       oci_status;
    char              *query, *buf;

    /* This indicator variable is a dummy.  We don't actually check the
     * status.  Previously, we set the indp parameter to OCIBindByPos to
     * 0.  Oracle would throw ORA-01405 and we would specifically ignore
     * it in tcl_error_p().  Now we pass this dummy variable, and Oracle
     * returns OCI_SUCCESS whether or not the returned value is NULL.
     * This eliminates the need for explicitly handling ORA-01405 in
     * tcl_error_p(). */

    sb2               null_indicator = OCI_IND_NULL;

    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 2, objv,
                "dbhandle dbId sql");
        return TCL_ERROR;
    }

    connection = dbh->connection;
    query = Tcl_GetString(objv[3]);

    if (!allow_sql_p(dbh, query, NS_TRUE)) {
        Tcl_AppendResult(interp, "SQL ", query, " has been rejected "
                         "by the Oracle driver", (char*)0L);
        return TCL_ERROR;
    }

    Ns_Log(Debug, "SQL():  %s", query);

    oci_status = OCIHandleAlloc (connection->env,
                                 (oci_handle_t **) &connection->stmt,
                                 OCI_HTYPE_STMT,
                                 0, NULL);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIHandleAlloc",
                    query, oci_status)) {
        Ns_OracleFlush (dbh);
        return TCL_ERROR;
    }

    oci_status = OCIStmtPrepare(connection->stmt,
                                connection->err,
                                (const OraText *)query,
                                (ub4) strlen(query),
                                OCI_NTV_SYNTAX,
                                OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtPrepare",
                    query, oci_status)) {
        Ns_OracleFlush (dbh);
        return TCL_ERROR;
    }

    buf = Ns_Malloc(EXEC_PLSQL_BUFFER_SIZE);

    oci_status = OCIBindByPos (connection->stmt,
                               &bind,
                               connection->err,
                               1,
                               buf,
                               EXEC_PLSQL_BUFFER_SIZE,
                               SQLT_STR,
                               &null_indicator,
                               0,
                               0,
                               0,
                               0,
                               OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIBindByPos",
                    query, oci_status)) {
        Ns_OracleFlush (dbh);
        Ns_Free(buf);

        return TCL_ERROR;
    }

    oci_status = OCIStmtExecute(connection->svc,
                                connection->stmt,
                                connection->err,
                                1, 0, NULL, NULL,
                                (connection->mode == autocommit ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT)
                                );
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtExecute",
                query, oci_status)) {
        Ns_OracleFlush (dbh);
        Ns_Free(buf);

        return TCL_ERROR;
    }

    Tcl_AppendResult(interp, buf, (char*)0L);
    Ns_Free(buf);

    return NS_OK;
}
/*}}}*/

/*{{{ OracleExecPLSQLBind
 *----------------------------------------------------------------------
 * OracleExecPLSQLBind --
 *
 *      Implements [ns_ora exec_plsql_bind]
 *
 *      ns_ora exec_plsql_bind dbhandle sql return_var ?arg1 ... argn?
 *
 * Results:
 *
 *      Nothing.
 *
 *----------------------------------------------------------------------
 */
int
OracleExecPLSQLBind(Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, Ns_DbHandle *dbh)
{
    ora_connection_t  *connection;
    oci_status_t       oci_status;
    string_list_elt_t *bind_variables, *var_p;
    int                argv_base, i;
    char               *retvar, *retbuf, *nbuf, *query;;

    if (objc < 5) {
        Tcl_AppendResult(interp, "wrong number of args: should be `",
                         Tcl_GetString(objv[0]),
                         " exec_plsql_bind dbId sql retvar <args>'", (char*)0L);
        return TCL_ERROR;
    }

    connection = dbh->connection;
    query = Tcl_GetString(objv[3]);
    retvar = Tcl_GetString(objv[4]);

    if (!allow_sql_p(dbh, query, NS_TRUE)) {
        Tcl_AppendResult(interp, "SQL ", query, " has been rejected "
                         "by the Oracle driver", (char*)0L);
        return TCL_ERROR;
    }

    Ns_Log(Debug, "SQL():  %s", query);

    oci_status = OCIHandleAlloc (connection->env,
                                 (oci_handle_t **) &connection->stmt,
                                 OCI_HTYPE_STMT,
                                 0, NULL);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIHandleAlloc",
                query, oci_status)) {
        Ns_OracleFlush (dbh);
        return TCL_ERROR;
    }

    oci_status = OCIStmtPrepare(connection->stmt,
                                connection->err,
                                (const OraText *)query,
                                (ub4) strlen(query),
                                OCI_NTV_SYNTAX,
                                OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtPrepare",
                query, oci_status)) {
        Ns_OracleFlush (dbh);
        return TCL_ERROR;
    }

    argv_base = 4;
    retbuf = NULL;

    bind_variables = parse_bind_variables(query);
    connection->n_columns = string_list_len(bind_variables);

    ns_ora_log(lexpos(), "%d bind variables", connection->n_columns);

    malloc_fetch_buffers (connection);

    for (var_p = bind_variables, i=0; var_p != NULL; var_p = var_p->next, i++) {
        fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];
        const char   *value = NULL;
        long            index;

        fetchbuf->type = (OCITypeCode)-1;
        index = strtol(var_p->string, &nbuf, 10);

        if (*nbuf == '\0') {
             /*  It was a valid number.
              *  Pick out one of the remaining arguments,
              *  where ":1" is the first remaining arg.
              */
            if ((index < 1) || (index > (objc - argv_base - 1))) {

                if (index < 1) {
                    Tcl_AppendResult(interp,
                                     "invalid positional variable `:",
                                     var_p->string,
                                     "', valid values start with 1", (char*)0L);
                } else {
                    Tcl_AppendResult(interp,
                                     "not enough arguments for positional variable ':",
                                     var_p->string, "'", (char*)0L);
                }

                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);

                return TCL_ERROR;
            }

            value = Tcl_GetString(objv[argv_base + index]);

        } else {

            value = Tcl_GetVar(interp, var_p->string, 0);

            if (value == NULL) {
                if (strcmp(var_p->string, retvar) == 0) {
                      /*  It's OK if it's undefined, since this is
                       *  the return variable.
                       */
                    value = "";
                } else {

                    Tcl_AppendResult(interp, "undefined variable `",
                                     var_p->string,
                                     "'", (char*)0L);

                    Ns_OracleFlush(dbh);
                    string_list_free_list(bind_variables);

                    return TCL_ERROR;
                }
            }
        }

        if (strcmp(var_p->string, retvar) == 0) {

            /*  This is the variable we're going to return
             *  as the result.
             */
            retbuf = fetchbuf->buf = ns_calloc(1, EXEC_PLSQL_BUFFER_SIZE);
            strncpy(retbuf, value, EXEC_PLSQL_BUFFER_SIZE);
            fetchbuf->fetch_length = EXEC_PLSQL_BUFFER_SIZE;
            fetchbuf->is_null = 0;

        } else {
            fetchbuf->buf = Ns_StrDup(value);
            fetchbuf->fetch_length = (ub2) strlen(fetchbuf->buf) + 1;
            fetchbuf->is_null = 0;
        }

        Ns_Log(Debug, "bind variable '%s' = '%s'", var_p->string, value);
        ns_ora_log(lexpos(), "ns_ora exec_plsql_bind:  binding variable %s",
                var_p->string);

        oci_status = OCIBindByName(connection->stmt,
                                   &fetchbuf->bind,
                                   connection->err,
                                   (const OraText *)var_p->string,
                                   (sb4) strlen(var_p->string),
                                   fetchbuf->buf,
                                   fetchbuf->fetch_length,
                                   SQLT_STR,
                                   &fetchbuf->is_null,
                                   0,
                                   0,
                                   0,
                                   0,
                                   OCI_DEFAULT);

        if (oci_error_p(lexpos(), dbh, "OCIBindByName", query, oci_status)) {
            Tcl_SetResult(interp, dbh->dsExceptionMsg.string, TCL_VOLATILE);
            Ns_OracleFlush (dbh);
            string_list_free_list(bind_variables);

            return TCL_ERROR;
        }

    }

    if (retbuf == NULL) {
        Tcl_AppendResult(interp, "return variable '", retvar,
                "' not found in statement bind variables", (char*)0L);
        Ns_OracleFlush (dbh);
        string_list_free_list(bind_variables);

        return TCL_ERROR;
    }

    oci_status = OCIStmtExecute(connection->svc,
                                connection->stmt,
                                connection->err,
                                1, 0, NULL, NULL,
                                (connection->mode == autocommit ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT)
                                );

    string_list_free_list(bind_variables);

    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtExecute", query, oci_status)) {
        Ns_OracleFlush (dbh);
        return TCL_ERROR;
    }

    Tcl_AppendResult(interp, retbuf, (char*)0L);

    /* Check to see if return variable was a Tcl variable */

    (void) strtol(retvar, &nbuf, 10);

    if (*nbuf != '\0') {
          /* It was a variable name. */
        Tcl_SetVar(interp, retvar, retbuf, 0);
    }

    return NS_OK;
}
/*}}}*/

/*{{{ OracleSelect
 *----------------------------------------------------------------------
 * OracleSelect --
 *
 *      Implements [ns_ora select]
 *                 [ns_ora dml]
 *                 [ns_ora array_dml]
 *                 [ns_ora 1row]
 *                 [ns_ora 0or1row]
 *
 *      ns_ora select    dbhandle sql
 *      ns_ora dml       dbhandle sql
 *      ns_ora array_dml dbhandle sql
 *      ns_ora 1row      dbhandle sql
 *      ns_ora 0or1row   dbhandle sql
 *
 * Results:
 *
 *      Nothing.
 *
 *----------------------------------------------------------------------
 */
int
OracleSelect(Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, Ns_DbHandle *dbh)
{
    ora_connection_t  *connection;
    oci_status_t       oci_status;
    string_list_elt_t *bind_variables,
                      *var_p;
    char              *query, *command, *subcommand;
    int                i;
    ub4                iters;
    ub2                type;
    int                dml_p;
    int                array_p;      /* Array DML */
    int                argv_base;    /* Index of the SQL statement argument (necessary to support -bind) */
    Ns_Set            *set = NULL;   /* If we're binding to an ns_set, a pointer to the struct */

    if (objc < 4 || (!strcmp("-bind", Tcl_GetString(objv[3])) && objc < 6)) {
        Tcl_WrongNumArgs(interp, 2, objv,
                "dbhandle ?-bind set? sql ?arg1 .. argN?");
        return TCL_ERROR;
    }

    command = Tcl_GetString(objv[0]);
    subcommand = Tcl_GetString(objv[1]);

    connection = dbh->connection;

    if (!connection) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("error: no connection", TCL_INDEX_NONE));
        return TCL_ERROR;
    }
    connection->interp = interp;

    if (!strcmp(subcommand, "dml")) {
        dml_p = 1;
        array_p = 0;
    } else if (!strcmp(subcommand, "array_dml")) {
        dml_p = 1;
        array_p = 1;
    } else {
        dml_p = 0;
        array_p = 0;
    }

    if (!strcmp("-bind", Tcl_GetString(objv[3]))) {
        /* Binding to a set. The query is argv[5]. */
        argv_base = 5;
        set = Ns_TclGetSet(interp, Tcl_GetString(objv[4]));
        if (set == NULL) {
            Tcl_AppendResult(interp, "invalid set id `", Tcl_GetString(objv[4]), "'", (char*)0L);
            return TCL_ERROR;
        }
    } else {
        /* Not binding to a set. The query is argv[3]. */
        argv_base = 3;
    }

    query = Tcl_GetString(objv[argv_base]);

    if (!allow_sql_p(dbh, query, NS_TRUE)) {
        Tcl_AppendResult(interp, "SQL ", query, " has been rejected "
                "by the Oracle driver", (char*)0L);
        return TCL_ERROR;
    }

    Ns_Log(Debug, "SQL():  %s", query);


    /* In order to handle transactions we check now for our
     * custom SQL-like commands.  If query is one of those
     * commands then we are done after calling handle_builtins.
     */
    switch (handle_builtins(dbh, query)) {
        case NS_DML:
            return TCL_OK;

        case NS_ERROR:
            Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                          TCL_VOLATILE);
            return TCL_ERROR;

        case NS_OK:
            break;

        default:
            error(lexpos(), "internal error");
            Tcl_AppendResult(interp, "internal error", (char*)0L);
            return TCL_ERROR;
    }

    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) &connection->stmt,
                                OCI_HTYPE_STMT, 0, NULL);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIHandleAlloc", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    oci_status = OCIStmtPrepare(connection->stmt,
                                connection->err,
                                (const OraText *)query,
                                (ub4) strlen(query),
                                OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtPrepare", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    /* Check what type of statement it is, this will affect how
     * many times we expect to execute it.
     */
    oci_status = OCIAttrGet(connection->stmt,
                            OCI_HTYPE_STMT,
                            (oci_attribute_t *) & type,
                            NULL, OCI_ATTR_STMT_TYPE, connection->err);
    if (oci_error_p(lexpos(), dbh, "OCIAttrGet", query, oci_status)) {
        Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                      TCL_VOLATILE);
        Ns_OracleFlush(dbh);
        return NS_ERROR;
    }

    /* For SELECT statements we can set a couple extra attributes
     * that may help speed up the query.
     */
    if (type == OCI_STMT_SELECT) {
        iters = 0;
        if (prefetch_rows > 0) {
            /* Set prefetch rows attr for selects. */
            oci_status = OCIAttrSet(connection->stmt,
                                    OCI_HTYPE_STMT,
                                    (dvoid *) & prefetch_rows,
                                    0,
                                    OCI_ATTR_PREFETCH_ROWS,
                                    connection->err);
            if (oci_error_p(lexpos(), dbh, "OCIAttrSet", query, oci_status)) {
                Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                              TCL_VOLATILE);
                Ns_OracleFlush(dbh);
                return NS_ERROR;
            }
        }
        if (prefetch_memory > 0) {
            /* Set prefetch memory attr for selects. */
            oci_status = OCIAttrSet(connection->stmt,
                                    OCI_HTYPE_STMT,
                                    (dvoid *) & prefetch_memory,
                                    0,
                                    OCI_ATTR_PREFETCH_MEMORY,
                                    connection->err);
            if (oci_error_p(lexpos(), dbh, "OCIAttrSet", query, oci_status)) {
                Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                              TCL_VOLATILE);
                Ns_OracleFlush(dbh);
                return NS_ERROR;
            }
        }
    } else {
        iters = 1;
    }

    /* Check for statement type mismatch */
    if (type != OCI_STMT_SELECT && !dml_p) {
        Ns_DbSetException(dbh, "ORA",
                "Query was not a statement returning rows.");
        Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                TCL_VOLATILE);
        Ns_OracleFlush(dbh);

        return TCL_ERROR;
    } else if (type == OCI_STMT_SELECT && dml_p) {
        Ns_DbSetException(dbh, "ORA",
                "Query was not a DML statement.");
        Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                TCL_VOLATILE);
        Ns_OracleFlush(dbh);

        return TCL_ERROR;
    }

    bind_variables = parse_bind_variables(query);
    connection->n_columns = string_list_len(bind_variables);

    ns_ora_log(lexpos(), "%d bind variables", connection->n_columns);

    if (connection->n_columns > 0) {
        malloc_fetch_buffers(connection);
    }

    /* Process bind variables.
     */
    for (var_p = bind_variables, i = 0; var_p != NULL;
            var_p = var_p->next, i++) {

        fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];
        char *nbuf;
        const char *value = NULL;
        long  index;
        size_t max_length = 0;

        fetchbuf->type = (OCITypeCode)-1;
        index = strtol(var_p->string, &nbuf, 10);

        /* Depending on how this proc was called we will get
         * the values used in binding from one of three places:
         * Tcl variable (if named bind), ns_set (if -bind was set), or
         * from the arguments (fi positional bind).
         */
        if (*nbuf == '\0') {

            /* It was a valid number.
             * Pick out one of the remaining arguments,
             * where ":1" is the first remaining arg.
             */

            if ((index < 1) || (index > (objc - argv_base - 1))) {

                if (index < 1) {
                    Tcl_AppendResult(interp,
                            "invalid positional variable `:",
                            var_p->string,
                            "', valid values start with 1",
                            (char*)0L);
                } else {
                    Tcl_AppendResult(interp,
                            "not enough arguments for positional variable ':",
                            var_p->string, "'", (char*)0L);
                }

                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                return TCL_ERROR;
            }

            value = Tcl_GetString(objv[index + argv_base]);

        } else {

            if (set == NULL) {

                /* Look for bind value in Tcl variable. */
                fetchbuf->name = var_p->string;
                value = Tcl_GetVar(interp, var_p->string, 0);

                if (value == NULL) {
                    Tcl_AppendResult(interp, "undefined variable `",
                            var_p->string, "'", (char*)0L);
                    Ns_OracleFlush(dbh);
                    string_list_free_list(bind_variables);
                    return TCL_ERROR;
                }

            } else {

                /* Look for bind value in Ns_Set. */
                //fetchbuf->name = var_p->string;
                value = Ns_SetGet(set, var_p->string);

                if (value == NULL) {
                    Tcl_AppendResult(interp, "undefined set element `",
                            var_p->string, "'", (char*)0L);
                    Ns_OracleFlush(dbh);
                    string_list_free_list(bind_variables);
                    return TCL_ERROR;
                }

            }
        }

        if (array_p) {
            int j;

            /*
             * We are using array dml so attempt to split the value
             * into a list.
             */

            if ((Tcl_SplitList(interp, value,
                               &fetchbuf->array_count,
                               &fetchbuf->array_values)) != TCL_OK) {
                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                return TCL_ERROR;
            }

            /*
             * All lists need to be of the same length, so we keep
             * track of that here.
             */

            if (i == 0) {
                iters = (ub4)fetchbuf->array_count;
            } else {

                if ((int) iters != fetchbuf->array_count) {
                    Tcl_AppendResult(interp,
                                     "non-matching numbers of rows",
                                     (char*)0L);
                    Ns_OracleFlush(dbh);
                    string_list_free_list(bind_variables);
                    return TCL_ERROR;
                }

            }

            for (j = 0; j < (int) iters; ++j) {
                unsigned int len = (unsigned int) strlen(fetchbuf->array_values[j]);
                if (len > max_length) {
                    max_length = len;
                }
            }

        } else if (dml_p) {
            fetchbuf->buf = Ns_Malloc(DML_BUFFER_SIZE);
            memset(fetchbuf->buf, (int) '\0', (size_t) DML_BUFFER_SIZE);
            strncpy(fetchbuf->buf, value, DML_BUFFER_SIZE);
            fetchbuf->fetch_length = DML_BUFFER_SIZE;
            fetchbuf->is_null = 0;
        } else {
            fetchbuf->buf = Ns_StrDup(value);
            fetchbuf->fetch_length = (ub2) strlen(fetchbuf->buf) + 1;
            fetchbuf->is_null = 0;
        }

        Ns_Log(Debug, "bind variable '%s' = '%s'", var_p->string, value);
        ns_ora_log(lexpos(), "ns_ora dml:  binding variable %s", var_p->string);

        if (array_p || dml_p) {
            oci_status = OCIBindByName(connection->stmt,
                                       &fetchbuf->bind,
                                       connection->err,
                                       (const OraText *)var_p->string,
                                       (sb4) strlen(var_p->string),
                                       NULL,
                                       array_p ? (sb4)max_length : fetchbuf->fetch_length,
                                       array_p ? SQLT_CHR : SQLT_STR,
                                       0, 0, 0, 0, 0,
                                       OCI_DATA_AT_EXEC);
        } else {
            oci_status = OCIBindByName(connection->stmt,
                                       &fetchbuf->bind,
                                       connection->err,
                                       (const OraText *)var_p->string,
                                       (sb4) strlen(var_p->string),
                                       fetchbuf->buf,
                                       fetchbuf->fetch_length,
                                       SQLT_STR,
                                       &fetchbuf->is_null,
                                       0, 0, 0, 0,
                                       OCI_DEFAULT);
        }

        if (oci_error_p(lexpos(), dbh, "OCIBindByName", query, oci_status)) {
            Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                          TCL_VOLATILE);
            Ns_OracleFlush(dbh);
            string_list_free_list(bind_variables);
            return TCL_ERROR;
        }

        if (array_p) {

            /* Array DML - dynamically bind, using list_element_put_data (which will
             * return the right item for each iteration).
             */
            oci_status = OCIBindDynamic(fetchbuf->bind,
                                        connection->err,
                                        fetchbuf, list_element_put_data,
                                        fetchbuf, get_data);
            if (tcl_error_p(lexpos(), interp, dbh, "OCIBindDynamic", query,
                 oci_status)) {
                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                return TCL_ERROR;
            }

        } else if (dml_p) {

            oci_status = OCIBindDynamic(fetchbuf->bind,
                                        connection->err,
                                        fetchbuf, DynamicBindIn,
                                        fetchbuf, DynamicBindOut);
            if (tcl_error_p(lexpos(), interp, dbh, "OCIBindDynamic", query,
                 oci_status)) {
                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                return TCL_ERROR;
            }
        }

    }

    ns_ora_log(lexpos(), "ns_ora dml:  executing statement %s", nilp(query));

    oci_status = OCIStmtExecute(connection->svc,
                                connection->stmt,
                                connection->err,
                                iters, 0, NULL, NULL,
                                OCI_DEFAULT);

    /*
     * Handle DML with "RETURNING INTO" clause.  Currently will
     * not work for array DML.
     */

    if (dml_p && !array_p) {
        for (var_p = bind_variables,
            i = 0; var_p != NULL;
             var_p = var_p->next, i++) {

            fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];

            if (fetchbuf->inout == BIND_OUT) {
                if (set == NULL) {
                    Tcl_SetVar(interp, var_p->string, fetchbuf->buf, 0);
                } else {
                    Ns_SetUpdate(set, var_p->string, fetchbuf->buf);
                }
            }
        }
    }

    string_list_free_list(bind_variables);
    if (connection->n_columns > 0) {
        if (connection->fetch_buffers != NULL) {
            for (i = 0; i < connection->n_columns; i++) {
                Ns_Free(connection->fetch_buffers[i].buf);
                connection->fetch_buffers[i].buf = NULL;
                Ns_Free(connection->fetch_buffers[i].array_values);
                if (connection->fetch_buffers[i].array_values != 0) {
                    ns_ora_log(lexpos(), "*** Freeing buffer %p",
                        connection->fetch_buffers[i].array_values);
                }
                connection->fetch_buffers[i].array_values = NULL;
            }
            Ns_Free(connection->fetch_buffers);
            connection->fetch_buffers = 0;
        }
    }

    if (oci_error_p(lexpos(), dbh, "OCIStmtExecute", query, oci_status)) {
        Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                      TCL_VOLATILE);
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    if (dml_p) {
        if (connection->mode == autocommit) {
            oci_status = OCITransCommit(connection->svc,
                                        connection->err, OCI_DEFAULT);
            if (oci_error_p(lexpos(), dbh, "OCITransCommit", query, oci_status)) {
                Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                              TCL_VOLATILE);
                Ns_OracleFlush(dbh);
                return TCL_ERROR;
            }
        }
    } else {

        Ns_Set *setPtr;
        Ns_TclSetType dynamic = NS_TCL_SET_STATIC;

        ns_ora_log(lexpos(), "ns_ora dml:  doing bind for select");
        Ns_SetTrunc(dbh->row, 0);
        setPtr = Ns_OracleBindRow(dbh);

        if (!strcmp(subcommand, "1row") ||
            !strcmp(subcommand, "0or1row")) {
            Ns_Set *row;
            int nrows;

            row = Oracle0or1Row(interp, dbh, setPtr, &nrows);
            dynamic = NS_TCL_SET_DYNAMIC;

            if (row != NULL) {
                if (!strcmp(subcommand, "1row") && nrows != 1) {
                    Ns_DbSetException(dbh, "ORA",
                                      "Query did not return a row.");
                    /* XXX doesn't this leak a row? */
                    row = NULL;
                    Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                                  TCL_VOLATILE);
                }
            }
            if (row == NULL) {
                error(lexpos(), "Database operation \"%s\" failed",
                      command);
                Ns_OracleFlush(dbh);
                return TCL_ERROR;
            }
            if (nrows == 0) {
                Ns_SetFree(row);
                row = NULL;
            }
            setPtr = row;
        }

        if (setPtr != NULL)
            Ns_TclEnterSet(interp, setPtr, dynamic);
    }

    return TCL_OK;
}
/*}}}*/

/*{{{ Oracle0or1Row */
/*----------------------------------------------------------------------
 * Ns_Oracle0or1Row --
 *
 *      Helper for [ns_ora 0or1row]
 *
 *----------------------------------------------------------------------
 */
static Ns_Set *
Oracle0or1Row(Tcl_Interp *interp, Ns_DbHandle *handle,
              Ns_Set *row, int *nrows)
{
    ns_ora_log(lexpos(), "entry");

    if (row != NULL) {
        if (Ns_OracleGetRow(handle, row) == NS_END_DATA) {
            *nrows = 0;
        } else {
            switch (Ns_OracleGetRow(handle, row)) {
            case NS_END_DATA:
                *nrows = 1;
                break;

            case NS_OK:
                Ns_DbSetException(handle, "ORA",
                                  "Query returned more than one row.");
                Tcl_SetResult(interp, handle->dsExceptionMsg.string,
                              TCL_VOLATILE);
                Ns_DbFlush(handle);
                /* FALLTHROUGH */

            case NS_ERROR:
                /* FALLTHROUGH */

            default:
                return NULL;
                break;
            }
        }
        row = Ns_SetCopy(row);
    }

    return row;
}
/*}}}*/

/*{{{ OracleLobDML */
/*----------------------------------------------------------------------
 * OracleLobDML --
 *
 *      Implements [ns_ora clob_dml]
 *                 [ns_ora clob_dml_file]
 *                 [ns_ora blob_dml]
 *                 [ns_ora blob_dml_file]
 *
 *      ns_ora clob_dml dbhandle      sql clob1 ?clob2 ... clobN?
 *      ns_ora clob_dml_file dbhandle sql path1 ?path2 ... pathN?
 *      ns_ora blob_dml dbhandle      sql blob1 ?blob2 ... blobN?
 *      ns_ora blob_dml_file dbhandle sql path1 ?path2 ... pathN?
 *
 * Results:
 *
 *      Nothing.
 *
 *----------------------------------------------------------------------
 */
int
OracleLobDML(Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, Ns_DbHandle *dbh)
{
    Tcl_Obj           *const*data;
    oci_status_t       oci_status;
    ora_connection_t  *connection;
    char              *query;
    sb4                k;
    sb4                colNum;
    int                files_p = NS_FALSE;
    int                blob_p = NS_FALSE;

    if (objc < 5) {
        Tcl_WrongNumArgs(interp, 2, objv, \
                "dbId query clobList [clobValues | filenames] ...");
        return TCL_ERROR;
    }

    connection = dbh->connection;
    query = Tcl_GetString(objv[3]);

    if (!strcmp(Tcl_GetString(objv[1]), "clob_dml_file") ||
        !strcmp(Tcl_GetString(objv[1]), "blob_dml_file"))
        files_p = NS_TRUE;

    if (!strncmp(Tcl_GetString(objv[1]), "blob", 4))
        blob_p = NS_TRUE;

    if (!allow_sql_p(dbh, query, NS_TRUE)) {
        Tcl_AppendResult(interp, "SQL ", query, " has been rejected "
                         "by the Oracle driver", (char*)0L);
        return TCL_ERROR;
    }

    Ns_Log(Debug, "SQL():  %s", query);

    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & connection->stmt,
                                OCI_HTYPE_STMT, 0, NULL);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIHandleAlloc", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    oci_status = OCIStmtPrepare(connection->stmt,
                                connection->err,
                                (const OraText *)query,
                                (ub4) strlen(query),
                                OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtPrepare", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    data = &objv[4];
    connection->n_columns = objc - 4;

    if (files_p) {
        for (colNum = 0; colNum < connection->n_columns; colNum++) {
            if (access(Tcl_GetString(data[colNum]), R_OK) != 0) {
                Tcl_AppendResult(interp, "could not access file",
                        Tcl_GetString(data[colNum]), (char*)0L);
                Ns_OracleFlush(dbh);
                return TCL_ERROR;
            }
        }
    }

    malloc_fetch_buffers(connection);

    for (colNum = 0; colNum < connection->n_columns; colNum++) {
        fetch_buffer_t *fetchbuf = &connection->fetch_buffers[colNum];

        fetchbuf->type = (OCITypeCode)-1;

        oci_status = OCIBindByPos(connection->stmt,
                                  &fetchbuf->bind,
                                  connection->err,
                                  (ub4)colNum + 1,
                                  0,
                                  -1,
                                  (blob_p) ? SQLT_BLOB : SQLT_CLOB,
                                  0, 0, 0, 0, 0, OCI_DATA_AT_EXEC);
        if (tcl_error_p(lexpos(), interp, dbh, "OCIBindByPos", query, oci_status)) {
            Ns_OracleFlush(dbh);
            return TCL_ERROR;
        }

        oci_status = OCIBindDynamic(fetchbuf->bind,
                                    connection->err,
                                    0, no_data, fetchbuf, get_data);
        if (tcl_error_p(lexpos(), interp, dbh, "OCIBindDynamic", query, oci_status)) {
            Ns_OracleFlush(dbh);
            return TCL_ERROR;
        }
    }

    oci_status = OCIStmtExecute(connection->svc,
                                connection->stmt,
                                connection->err,
                                1, 0, NULL, NULL, OCI_DEFAULT);

    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtExecute", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }


    for (colNum = 0; colNum < connection->n_columns; colNum++) {
        fetch_buffer_t *fetchbuf = &connection->fetch_buffers[colNum];
        ub4 length = (ub4)-1;

        if (files_p) {
            Ns_Log(Debug, "  CLOB # %d, filename %s", colNum,
                   Tcl_GetString(data[colNum]));
        } else {
            length = (ub4) strlen(Tcl_GetString(data[colNum]));
            Ns_Log(Debug, "  CLOB # %d, length %d: %s", colNum, length,
                   (length == 0) ? "(NULL)" :
                   Tcl_GetString(data[colNum]));
        }

        /* if length is zero, that's an empty string.  Bypass the LobWrite
         * to have it insert a NULL value
         */
        if (length == 0)
            continue;

        for (k = 0; k < (sb4)fetchbuf->n_rows; k++) {
            if (files_p) {
                if (stream_read_lob
                    (interp, dbh, 1, fetchbuf->lobs[k], Tcl_GetString(data[colNum]),
                     connection)
                    != NS_OK) {
                    tcl_error_p(lexpos(), interp, dbh, "stream_read_lob",
                                query, oci_status);
                    return TCL_ERROR;
                }
                continue;
            }

            oci_status = OCILobWrite(connection->svc,
                                     connection->err,
                                     fetchbuf->lobs[k],
                                     &length,
                                     1,
                                     Tcl_GetString(data[colNum]),
                                     length,
                                     OCI_ONE_PIECE, 0, 0, 0,
                                     SQLCS_IMPLICIT);

            if (tcl_error_p(lexpos(), interp, dbh, "OCILobWrite", query,
                 oci_status)) {
                Ns_OracleFlush(dbh);
                return TCL_ERROR;
            }
        }
    }

    if (connection->mode == autocommit) {
        oci_status = OCITransCommit(connection->svc,
                                    connection->err, OCI_DEFAULT);
        if (tcl_error_p(lexpos(), interp, dbh, "OCITransCommit", query, oci_status)) {
            Ns_OracleFlush(dbh);
            return TCL_ERROR;
        }
    }

    /* all done */
    free_fetch_buffers(connection);

    return TCL_OK;
}
/*}}}*/

/*{{{ OracleLobDMLBind*/
/*----------------------------------------------------------------------
 * OracleLobDMLBind --
 *
 *      Implements [ns_ora clob_dml_bind]
 *                 [ns_ora clob_dml_file_bind]
 *                 [ns_ora blob_dml_bind]
 *                 [ns_ora blob_dml_file_bind]
 *
 *      ns_ora clob_dml_bind      dbhandle sql list_of_lobs ?clob1 ... clobN?
 *      ns_ora clob_dml_file_bind dbhandle sql list_of_lobs ?clob1 ... clobN?
 *      ns_ora blob_dml_bind      dbhandle sql list_of_lobs ?blob1 ... blobN?
 *      ns_ora blob_dml_file_bind dbhandle sql list_of_lobs ?clob1 ... clobN?
 *
 * Results:
 *
 *      Nothing.
 *
 *----------------------------------------------------------------------
 */
int
OracleLobDMLBind(Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, Ns_DbHandle *dbh)
{
    oci_status_t       oci_status;
    ora_connection_t  *connection;
    string_list_elt_t *bind_variables, *var_p;
    char              *query;
    int                i,k;
    int                files_p = NS_FALSE;
    int                blob_p = NS_FALSE;
    const char       **lob_argv;
    TCL_SIZE_T         lob_argc;
    int                argv_base;

    if (objc < 5) {
        Tcl_WrongNumArgs(interp, 2, objv, \
                "dbId query clobList [clobValues | filenames] ...");
        return TCL_ERROR;
    }

    query = Tcl_GetString(objv[3]);
    connection = dbh->connection;

    if (!strcmp(Tcl_GetString(objv[1]), "clob_dml_file_bind") ||
        !strcmp(Tcl_GetString(objv[1]), "blob_dml_file_bind"))
        files_p = NS_TRUE;

    if (!strncmp(Tcl_GetString(objv[1]), "blob", 4))
        blob_p = NS_TRUE;

    if (!allow_sql_p(dbh, query, NS_TRUE)) {
        Tcl_AppendResult(interp, "SQL ", query, " has been rejected "
                         "by the Oracle driver", (char*)0L);
        return TCL_ERROR;
    }

    Ns_Log(Debug, "SQL():  %s", query);

    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & connection->stmt,
                                OCI_HTYPE_STMT, 0, NULL);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIHandleAlloc", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    oci_status = OCIStmtPrepare(connection->stmt,
                                connection->err,
                                (const OraText *)query,
                                (ub4) strlen(query),
                                OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtPrepare", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    //connection->n_columns = objc - 4;

    Tcl_SplitList(interp, Tcl_GetString(objv[4]), &lob_argc, &lob_argv);

    bind_variables = parse_bind_variables(query);

    connection->n_columns = string_list_len(bind_variables);

    ns_ora_log(lexpos(), "%d bind variables", connection->n_columns);

    malloc_fetch_buffers(connection);

    argv_base = 4;

    for (var_p = bind_variables, i = 0; var_p != NULL;
         var_p = var_p->next, i++) {

        fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];
        char           *nbuf;
        const char   *value = NULL;
        int             lob_i;
        long            index;

        fetchbuf->type = (OCITypeCode)-1;
        index = strtol(var_p->string, &nbuf, 10);
        if (*nbuf == '\0') {
            /* It was a valid number.
               Pick out one of the remaining arguments,
               where ":1" is the first remaining arg. */
            if ((index < 1) || (index > (objc - argv_base - 1))) {
                if (index < 1) {
                    Tcl_AppendResult(interp,
                                     "invalid positional variable `:",
                                     var_p->string,
                                     "', valid values start with 1", (char*)0L);
                } else {
                    Tcl_AppendResult(interp,
                                     "not enough arguments for positional variable ':",
                                     var_p->string, "'", (char*)0L);
                }
                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                Tcl_Free((char *) lob_argv);
                return TCL_ERROR;
            }
            value = Tcl_GetString(objv[argv_base + index]);
        } else {
            value = Tcl_GetVar(interp, var_p->string, 0);
            if (value == NULL) {
                Tcl_AppendResult(interp, "undefined variable `",
                                 var_p->string, "'", (char*)0L);
                Ns_OracleFlush(dbh);
                string_list_free_list(bind_variables);
                Tcl_Free((char *) lob_argv);
                return TCL_ERROR;
            }
        }


        fetchbuf->buf = Ns_StrDup(value);
        fetchbuf->fetch_length = (ub2) strlen(fetchbuf->buf) + 1;
        fetchbuf->is_null = 0;

        Ns_Log(Debug, "bind variable '%s' = '%s'", var_p->string, value);
        ns_ora_log(lexpos(), "ns_ora clob_dml:  binding variable %s",
            var_p->string);

        for (lob_i = 0; lob_i < lob_argc; lob_i++) {
            if (strcmp(lob_argv[lob_i], var_p->string) == 0) {
                fetchbuf->is_lob = 1;
                ns_ora_log(lexpos(), "bind variable %s is a lob", var_p->string);
                break;
            }
        }

        oci_status = OCIBindByName(connection->stmt,
                                   &fetchbuf->bind,
                                   connection->err,
                                   (const OraText *)var_p->string,
                                   (sb4) strlen(var_p->string),
                                   fetchbuf->buf,
                                   fetchbuf->fetch_length,
                                   fetchbuf->is_lob ?
                                   (blob_p ? SQLT_BLOB : SQLT_CLOB)
                                   : SQLT_STR,
                                   &fetchbuf->is_null,
                                   0,
                                   0,
                                   0,
                                   0,
                                   fetchbuf->is_lob ?
                                   OCI_DATA_AT_EXEC : OCI_DEFAULT);

        if (oci_error_p(lexpos(), dbh, "OCIBindByName", query, oci_status)) {
            Tcl_SetResult(interp, dbh->dsExceptionMsg.string,
                          TCL_VOLATILE);
            Ns_OracleFlush(dbh);
            string_list_free_list(bind_variables);
            Tcl_Free((char *) lob_argv);
            return TCL_ERROR;
        }

        if (fetchbuf->is_lob) {
            oci_status = OCIBindDynamic(fetchbuf->bind,
                                        connection->err,
                                        0, no_data, fetchbuf, get_data);
            if (tcl_error_p(lexpos(), interp, dbh, "OCIBindDynamic", query,
                            oci_status)) {
                Ns_OracleFlush(dbh);
                Tcl_Free((char *) lob_argv);
                string_list_free_list(bind_variables);
                return TCL_ERROR;
            }
        }
    }

    Tcl_Free((char *) lob_argv);

    oci_status = OCIStmtExecute(connection->svc,
                                connection->stmt,
                                connection->err,
                                1, 0, NULL, NULL, OCI_DEFAULT);

    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtExecute", query, oci_status)) {
        Ns_OracleFlush(dbh);
        string_list_free_list(bind_variables);
        return TCL_ERROR;
    }

    for (var_p = bind_variables, i = 0; var_p != NULL;
         var_p = var_p->next, i++) {

        fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];
        ub4             length = (ub4)-1;

        if (!fetchbuf->is_lob) {
            ns_ora_log(lexpos(), "column %d is not a lob", i);
            continue;
        }


        if (files_p) {
            Ns_Log(Debug, "  CLOB # %d, filename %s", i, fetchbuf->buf);
        } else {
            length = (ub4) strlen(fetchbuf->buf);
            Ns_Log(Debug, "  CLOB # %d, length %d: %s", i, length,
                   (length == 0) ? "(NULL)" : fetchbuf->buf);
        }

        /* if length is zero, that's an empty string.  Bypass the LobWrite
         * to have it insert a NULL value
         */
        if (length == 0)
            continue;

        for (k = 0; k < (int) fetchbuf->n_rows; k++) {
            if (files_p) {
                if (stream_read_lob
                    (interp, dbh, 1, fetchbuf->lobs[k], fetchbuf->buf,
                     connection)
                    != NS_OK) {
                    tcl_error_p(lexpos(), interp, dbh, "stream_read_lob",
                                query, oci_status);
                    string_list_free_list(bind_variables);
                    return TCL_ERROR;
                }
                continue;
            }

            ns_ora_log(lexpos(), "using lob %x", fetchbuf->lobs[k]);
            oci_status = OCILobWrite(connection->svc,
                                     connection->err,
                                     fetchbuf->lobs[k],
                                     &length,
                                     1,
                                     fetchbuf->buf,
                                     length,
                                     OCI_ONE_PIECE, 0, 0, 0,
                                     SQLCS_IMPLICIT);

            if (tcl_error_p(lexpos(), interp, dbh, "OCILobWrite", query,
                            oci_status)) {
                string_list_free_list(bind_variables);
                Ns_OracleFlush(dbh);
                return TCL_ERROR;
            }
        }
    }

    if (connection->mode == autocommit) {
        oci_status = OCITransCommit(connection->svc,
                                    connection->err, OCI_DEFAULT);
        if (tcl_error_p(lexpos(), interp, dbh, "OCITransCommit", query, oci_status)) {
            string_list_free_list(bind_variables);
            Ns_OracleFlush(dbh);
            return TCL_ERROR;
        }
    }

    /* all done */
    free_fetch_buffers(connection);
    string_list_free_list(bind_variables);

    return TCL_OK;
}
/*}}}*/

/*{{{ OracleLobSelect
 *----------------------------------------------------------------------
 * OracleLobSelect --
 *
 *      Implements [ns_ora clob_get_file]
 *                 [ns_ora blob_get_file]
 *                 [ns_ora write_clob]
 *                 [ns_ora write_blob]
 *
 *      ns_ora clob_get_file dbhandle sql path
 *      ns_ora blob_get_file dbhandle sql path
 *      ns_ora write_clob    dbhandle sql ?nbytes?
 *      ns_ora write_blob    dbhandle sql ?nbytes?
 *
 * Results:
 *
 *      Nothing.
 *
 *----------------------------------------------------------------------
 */
int
OracleLobSelect(Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, Ns_DbHandle *dbh)
{
    oci_status_t       oci_status;
    ora_connection_t  *connection;
    OCILobLocator     *lob = NULL;
    OCIDefine         *def;
    char              *query;
    char              *filename = NULL;
    char              *subcommand;
    int                blob_p = NS_FALSE;
    int                to_conn_p = NS_FALSE;
    int                nbytes = INT_MAX;
    int                result = TCL_ERROR;
    int                write_lob_status = NS_ERROR;

    if (objc < 4 ) {
        Tcl_WrongNumArgs(interp, 2, objv, "dbhandle ?-bind set? sql ?ref?");
        return TCL_ERROR;
    }

    subcommand = Tcl_GetString(objv[1]);
    connection = dbh->connection;

    if (!strncmp(subcommand, "write", 5))
        to_conn_p = NS_TRUE;

    if (to_conn_p) {
        if (objc < 4 || objc > 5) {
            Tcl_AppendResult(interp,
                             "wrong number of args: should be '",
                             Tcl_GetString(objv[0]),
                             subcommand, " dbId query ?nbytes?",
                             (char*)0L);
            goto write_lob_cleanup;
        }

        if (objc == 5) {
            if (Tcl_GetIntFromObj(interp, objv[4], &nbytes) != TCL_OK) {
                goto write_lob_cleanup;
            }
        }
    } else {
        if (objc != 5) {
            Tcl_AppendResult(interp,
                             "wrong number of args: should be '",
                             Tcl_GetString(objv[0]),
                             subcommand, " dbId query filename",
                             (char*)0L);
            goto write_lob_cleanup;
        }
    }

    if (!strncmp(subcommand, "blob", 4) || !strcmp(subcommand, "write_blob"))
        blob_p = NS_TRUE;

    query = Tcl_GetString(objv[3]);

    if (!allow_sql_p(dbh, query, NS_TRUE)) {
        Tcl_AppendResult(interp, "SQL ", query, " has been rejected "
                         "by the Oracle driver", (char*)0L);
        goto write_lob_cleanup;
    }

    Ns_Log(Debug, "SQL():  %s", query);

    oci_status = OCIDescriptorAlloc(connection->env,
                                    (dvoid **) & lob,
                                    OCI_DTYPE_LOB, 0, 0);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIDescriptorAlloc",
                    query, oci_status)) {
        goto write_lob_cleanup;
    }

    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & connection->stmt,
                                OCI_HTYPE_STMT, 0, NULL);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIHandleAlloc", query, oci_status)) {
        goto write_lob_cleanup;
    }


    oci_status = OCIStmtPrepare(connection->stmt,
                                connection->err,
                                (const OraText *)query,
                                (ub4) strlen(query),
                                OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtPrepare", query, oci_status)) {
        goto write_lob_cleanup;
    }

    oci_status = OCIDefineByPos(connection->stmt,
                                &def,
                                connection->err,
                                1,
                                &lob,
                                -1,
                                (blob_p) ? SQLT_BLOB : SQLT_CLOB,
                                0, 0, 0, OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtPrepare",
                    query, oci_status)) {
        goto write_lob_cleanup;
    }


    oci_status = OCIStmtExecute(connection->svc,
                                connection->stmt,
                                connection->err,
                                1, 0, 0, 0, OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtExecute",
                    query, oci_status)) {
        goto write_lob_cleanup;
    }

    if (!to_conn_p) {
        filename = Tcl_GetString(objv[4]);
    }

    write_lob_status =
        stream_write_lob(interp, dbh, 0, lob, filename, to_conn_p,
                         connection->svc, connection->err);
    if (write_lob_status == STREAM_WRITE_LOB_ERROR) {
        tcl_error_p(lexpos(), interp, dbh, "stream_write_lob",
                    query, oci_status);
        goto write_lob_cleanup;
    }

    /* if we survived to here, we're golden */
    result = TCL_OK;

  write_lob_cleanup:

    if (lob != NULL) {
        oci_status = OCIDescriptorFree(lob, OCI_DTYPE_LOB);
        oci_error_p(lexpos(), dbh, "OCIDescriptorFree", 0, oci_status);
    }

    Ns_OracleFlush(dbh);

    /* this is a hack.  If we don't drain a multi-part LOB, we'll get
     * an error next time we use the handle.  This works around the problem
     * for now until we find a better cleanup mechanism
     */
    if (write_lob_status != NS_OK) {
        Ns_OracleCloseDb(dbh);
        Ns_OracleOpenDb(dbh);
    }

    return result;
}
/*}}}*/

/*{{{ OracleGetCols
 *----------------------------------------------------------------------
 * OracleGetCols --
 *
 *      Implements [ns_ora getcols] command.
 *
 *      ns_ora getcols dbhandle sql
 *
 * Results:
 *
 *      Tcl list of columns in select-list.
 *
 *----------------------------------------------------------------------
 */
int
OracleGetCols(Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, Ns_DbHandle *dbh)
{
    ora_connection_t  *connection;
    oci_status_t       oci_status;
    char              *query;
    int                i;

    if (objc < 4) {
        Tcl_AppendResult(interp, "wrong number of args: should be `",
                         Tcl_GetString(objv[0]), " getcols dbId sql'", (char*)0L);
        return TCL_ERROR;
    }

    query = Tcl_GetString(objv[3]);

    connection = dbh->connection;
    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & connection->stmt,
                                OCI_HTYPE_STMT, 0, NULL);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIHandleAlloc", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    oci_status = OCIStmtPrepare(connection->stmt,
                                connection->err,
                                (const OraText *)query,
                                (ub4) strlen(query),
                                OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtPrepare", query, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    /* Execute Query in DESCRIBE_ONLY mode. */
    oci_status = OCIStmtExecute(connection->svc,
                                connection->stmt,
                                connection->err,
                                1, 0, 0, 0, OCI_DESCRIBE_ONLY);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIStmtExecute",
                    query, oci_status)) {
        return TCL_ERROR;
    }

    /* Get total number of columns. */
    oci_status = OCIAttrGet(connection->stmt,
                            OCI_HTYPE_STMT,
                            (oci_attribute_t *) & connection->n_columns,
                            NULL, OCI_ATTR_PARAM_COUNT,
                            connection->err);

    for (i = 0; i < connection->n_columns; i++) {
        OCIParam *param;
        char name[512];
        char *name1 = 0;
        ub2 coltype;
        ub4 name1_size = 0;
        sword result;

        result = OCIParamGet(connection->stmt,
                                 OCI_HTYPE_STMT,
                                 connection->err,
                             (oci_param_t *) & param, (ub4)i + 1);
        if (oci_error_p(lexpos(), dbh, "OCIParamGet", 0, result)) {
            Ns_OracleFlush(dbh);
            return TCL_ERROR;
        }

        oci_status = OCIAttrGet(param,
                                OCI_DTYPE_PARAM,
                                (oci_attribute_t *) & name1,
                                &name1_size,
                                OCI_ATTR_NAME, connection->err);
        if (oci_error_p(lexpos(), dbh, "OCIAttrGet", 0, oci_status)) {
            Ns_OracleFlush(dbh);
            return TCL_ERROR;
        }

        oci_status = OCIAttrGet(param,
                                OCI_DTYPE_PARAM,
                                (oci_attribute_t *) &coltype,
                                0,
                                OCI_ATTR_DATA_TYPE, connection->err);
        if (oci_error_p(lexpos(), dbh, "OCIAttrGet", 0, oci_status)) {
            Ns_OracleFlush(dbh);
            return TCL_ERROR;
        }

        /* Oracle gives us back a pointer to a string that is not null-terminated
         * so we copy it into our local var and add a 0 at the end.
         */
        memcpy(name, name1, name1_size);
        name[name1_size] = 0;
        downcase(name);

        Tcl_ListObjAppendElement(interp,
                                 Tcl_GetObjResult(interp), Tcl_NewIntObj(coltype));

        Tcl_ListObjAppendElement(interp,
                                 Tcl_GetObjResult(interp), Tcl_NewStringObj(name, (TCL_SIZE_T)name1_size));

    }

    Ns_OracleFlush(dbh);

    return TCL_OK;
}
/*}}}*/

/*{{{ OracleResultRows
 *----------------------------------------------------------------------
 * OracleResultRows --
 *
 *      Implements [ns_ora resultrows] command.
 *
 *      ns_ora resultrows dbhandle sql
 *
 * Results:
 *
 *      ...
 *
 *----------------------------------------------------------------------
 */
int
OracleResultRows(Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, Ns_DbHandle *dbh)
{
    ora_connection_t  *connection;
    oci_status_t       oci_status;
    ub4                count;
    char               buf[1024];

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "dbId object");
        return TCL_ERROR;
    }

    connection = dbh->connection;
    if (connection->stmt == 0) {
        Tcl_AppendResult(interp, "no active statement", (char*)0L);
        return TCL_ERROR;
    }

    oci_status = OCIAttrGet(connection->stmt,
                            OCI_HTYPE_STMT,
                            (oci_attribute_t *) &count,
                            NULL, OCI_ATTR_ROW_COUNT, connection->err);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", 0, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    snprintf(buf, 1024, "%ld", (long) count);
    Tcl_AppendResult(interp, buf, (char*)0L);

    return TCL_OK;
}
/*}}}*/

/*{{{ OracleDesc
 *----------------------------------------------------------------------
 * OracleDesc --
 *
 *      Implements [ns_oracle desc] command.
 *
 *      ns_oracle desc_pkg dbhandle object_name
 *
 *----------------------------------------------------------------------
 */
int
OracleDesc(Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, Ns_DbHandle *dbh)
{
    ora_connection_t  *connection;
    oci_status_t       oci_status;
    OCIDescribe       *descHandlePtr;
    OCIParam          *paramHandlePtr;
    char              *package;
    ub1                ptype;
    int                resolve;

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "dbhandle package");
        return TCL_ERROR;
    }

    if (objc == 5) {
        Tcl_GetIntFromObj(interp, objv[4], &resolve);
    } else {
        resolve = 1;
    }

    connection = dbh->connection;
    if (connection == NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("error: no connection", TCL_INDEX_NONE));
        return TCL_ERROR;
    }

    package = Tcl_GetString(objv[3]);

    oci_status = OCIHandleAlloc(connection->env,
                                (dvoid *)&descHandlePtr,
                                OCI_HTYPE_DESCRIBE, 0, NULL);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIHandleAlloc", package, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    oci_status = OCIAttrSet(descHandlePtr,
                            OCI_HTYPE_DESCRIBE,
                            (dvoid *) 1, 0,
                            OCI_ATTR_DESC_PUBLIC,
                            connection->err);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrSet", package, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    oci_status = OCIDescribeAny(connection->svc,
                                connection->err,
                                package,
                                (ub4) strlen(package),
                                OCI_OTYPE_NAME,
                                (ub1) 0,
                                OCI_PTYPE_UNK,
                                descHandlePtr);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIDescribeAny", package, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    /* Get parameter handle */
    oci_status = OCIAttrGet(descHandlePtr,
                            OCI_HTYPE_DESCRIBE,
                            &paramHandlePtr,
                            (ub4 *)0,
                            OCI_ATTR_PARAM,
                            connection->err);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", package, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    oci_status = OCIAttrGet(paramHandlePtr,
                            OCI_DTYPE_PARAM,
                            &ptype,
                            (ub4 *)0,
                            OCI_ATTR_PTYPE,
                            connection->err);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", package, oci_status)) {
        Ns_OracleFlush(dbh);
        return TCL_ERROR;
    }

    /* Resolve SYNONYM if necessary */
    if (ptype == OCI_PTYPE_SYN && resolve) {
        const char *syn_name, *syn_schema;
        char *syn_points_to, *syn_ptr;
        ub4   syn_name_len, syn_schema_len;

        oci_status = OCIAttrGet(paramHandlePtr,
                                OCI_DTYPE_PARAM,
                                (dvoid *) &syn_name,
                                (ub4 *)   &syn_name_len,
                                OCI_ATTR_NAME,
                                connection->err);
        if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", package, oci_status)) {
            Ns_OracleFlush(dbh);
            return TCL_ERROR;
        }

        oci_status = OCIAttrGet(paramHandlePtr,
                                OCI_DTYPE_PARAM,
                                (dvoid *) &syn_schema,
                                (ub4 *)   &syn_schema_len,
                                OCI_ATTR_SCHEMA_NAME,
                                connection->err);
        if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", package, oci_status)) {
            Ns_OracleFlush(dbh);
            return TCL_ERROR;
        }

        syn_points_to = ns_malloc(syn_schema_len+syn_name_len+4);
        strncpy(syn_points_to, syn_schema, syn_schema_len);
        syn_ptr = syn_points_to + syn_schema_len;
        *syn_ptr++ = '.';
        strncpy(syn_ptr, syn_name, syn_name_len);
        syn_ptr += syn_name_len;

        *syn_ptr = '\0';

        oci_status = OCIDescribeAny(connection->svc,
                                    connection->err,
                                    (text *) syn_points_to,
                                    (ub4) (syn_ptr - syn_points_to),
                                    OCI_OTYPE_NAME,
                                    (ub1) 0,
                                    OCI_PTYPE_UNK,
                                    descHandlePtr);
        if (tcl_error_p(lexpos(), interp, dbh, "OCIDescribeAny", package, oci_status)) {
            Ns_OracleFlush(dbh);
            OCIHandleFree(descHandlePtr, OCI_HTYPE_DESCRIBE);
            return TCL_ERROR;
        }

        ns_free(syn_points_to);

        /* Get parameter handle */
        oci_status = OCIAttrGet(descHandlePtr,
                                OCI_HTYPE_DESCRIBE,
                                &paramHandlePtr,
                                (ub4 *)0,
                                OCI_ATTR_PARAM,
                                connection->err);
        if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", package, oci_status)) {
            Ns_OracleFlush(dbh);
            OCIHandleFree(descHandlePtr, OCI_HTYPE_DESCRIBE);
            return TCL_ERROR;
        }
    }

    oci_status = OCIAttrGet(paramHandlePtr,
                            OCI_DTYPE_PARAM,
                            &ptype,
                            (ub4 *)0,
                            OCI_ATTR_PTYPE,
                            connection->err);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", package, oci_status)) {
        Ns_OracleFlush(dbh);
        OCIHandleFree(descHandlePtr, OCI_HTYPE_DESCRIBE);
        return TCL_ERROR;
    }

    switch (ptype) {

        case OCI_PTYPE_PKG:
            OracleDescribePackage(descHandlePtr,
                                  paramHandlePtr,
                                  connection, dbh, package,
                                  interp);
            break;

        case OCI_PTYPE_SYN:
            OracleDescribeSynonym(descHandlePtr,
                                  paramHandlePtr,
                                  connection, dbh,
                                  interp);
            break;

        default:
            Ns_Log(Warning, "No desc handler, unable to describe object. %d", ptype);
    }

    OCIHandleFree(descHandlePtr, OCI_HTYPE_DESCRIBE);
    OCIHandleFree(paramHandlePtr, OCI_DTYPE_PARAM);

    return NS_OK;
}
/*}}}*/

/*{{{ OracleDescribeSynonym */
void
OracleDescribeSynonym(OCIDescribe *UNUSED(descHandlePtr),
                      OCIParam    *paramHandlePtr,
                      ora_connection_t *connection,
                      Ns_DbHandle *dbh,
                      Tcl_Interp  *interp)
{
    oci_status_t       oci_status;
    const char *syn_name, *syn_schema;
    ub4   syn_name_len, syn_schema_len;

    oci_status = OCIAttrGet(paramHandlePtr,
                            OCI_DTYPE_PARAM,
                            (dvoid *) &syn_name,
                            (ub4 *)   &syn_name_len,
                            OCI_ATTR_NAME,
                            connection->err);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", "", oci_status)) {
        Ns_OracleFlush(dbh);
        return;
    }

    oci_status = OCIAttrGet(paramHandlePtr,
                            OCI_DTYPE_PARAM,
                            (dvoid *) &syn_schema,
                            (ub4 *)   &syn_schema_len,
                            OCI_ATTR_SCHEMA_NAME,
                            connection->err);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", "", oci_status)) {
        Ns_OracleFlush(dbh);
        return;
    }

    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
                             Tcl_NewStringObj(syn_schema, (TCL_SIZE_T)syn_schema_len));
    Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp),
                             Tcl_NewStringObj(syn_name, (TCL_SIZE_T)syn_name_len));

}
/*}}}*/

/*{{{ OracleDescribePackage */
void
OracleDescribePackage(OCIDescribe      *descHandlePtr,
                      OCIParam         *paramHandlePtr,
                      ora_connection_t *connection,
                      Ns_DbHandle      *dbh,
                      char             *UNUSED(package),
                      Tcl_Interp       *interp)
{
    OCIParam     *procListPtr, *arg, *arg1;
    oci_status_t  oci_status;
    ub2           numProcs;
    ub4           namelen;
    const char   *name;
    int           i;

    oci_status = OCIAttrGet (paramHandlePtr, OCI_DTYPE_PARAM, &procListPtr, 0,
                             OCI_ATTR_LIST_SUBPROGRAMS, connection->err);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", "", oci_status)) {
        Ns_OracleFlush(dbh);
        return;
    }

    oci_status = OCIAttrGet (procListPtr, OCI_DTYPE_PARAM, &numProcs, 0, OCI_ATTR_NUM_PARAMS, connection->err);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", "", oci_status)) {
        Ns_OracleFlush(dbh);
        return;
    }

    for (i = 0; i < numProcs; i++) {
        Tcl_Obj *procObj = Tcl_NewObj();

        OCIParamGet (procListPtr, OCI_DTYPE_PARAM, connection->err, (dvoid *)&arg, (ub4)i);
        OCIAttrGet (arg, OCI_DTYPE_PARAM, &name, &namelen, OCI_ATTR_NAME, connection->err);
        OCIAttrGet (arg, OCI_DTYPE_PARAM, &arg1, 0, OCI_ATTR_LIST_ARGUMENTS, connection->err);

        Tcl_ListObjAppendElement(interp, procObj, Tcl_NewStringObj(name, (TCL_SIZE_T)namelen));

        OracleDescribeArguments(descHandlePtr, arg1, connection, dbh, interp, procObj);
    }

}
/*}}}*/

/*{{{ OracleDescribeArguments */
void
OracleDescribeArguments (OCIDescribe       *UNUSED(descHandlePtr),
                         OCIParam          *paramHandlePtr,
                         ora_connection_t  *connection,
                         Ns_DbHandle       *dbh,
                         Tcl_Interp        *interp,
                         Tcl_Obj           *list)
{
    OCIParam         *arg;
    OCITypeParamMode  mode;
    OCIParam         *arglst1;
    OCITypeCode       data_type;
    ub1               has_default;
    oci_status_t      oci_status;
    ub2               numargs;
    ub4               namelen;
    const char       *name;
    ub4               i;
    Tcl_Obj          *argObj = Tcl_NewObj();

    oci_status = OCIAttrGet (paramHandlePtr, OCI_DTYPE_PARAM, &numargs, 0, OCI_ATTR_NUM_PARAMS, connection->err);
    if (tcl_error_p(lexpos(), interp, dbh, "OCIAttrGet", "", oci_status)) {
        Ns_OracleFlush(dbh);
        return;
    }

    for (i = 0; i < numargs; i++) {
        Tcl_Obj *argument = Tcl_NewObj();

        oci_status = OCIParamGet (paramHandlePtr, OCI_DTYPE_PARAM, connection->err, (dvoid *)&arg, i);
        if (oci_status == OCI_ERROR) {
            numargs++;
            continue;
        }

        OCIAttrGet (arg, OCI_DTYPE_PARAM, &name, &namelen, OCI_ATTR_NAME, connection->err);
        OCIAttrGet (arg, OCI_DTYPE_PARAM, &arglst1, 0, OCI_ATTR_LIST_ARGUMENTS, connection->err);
        OCIAttrGet (arg, OCI_DTYPE_PARAM, &mode, 0, OCI_ATTR_IOMODE, connection->err);
        OCIAttrGet (arg, OCI_DTYPE_PARAM, &data_type, 0, OCI_ATTR_DATA_TYPE, connection->err);
        OCIAttrGet (arg, OCI_DTYPE_PARAM, &has_default, 0, OCI_ATTR_HAS_DEFAULT, connection->err);

        Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj(name, (TCL_SIZE_T)namelen));

        switch ((int)mode) {
            case OCI_TYPEPARAM_IN:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("IN", -1));
                break;

            case OCI_TYPEPARAM_OUT:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("OUT", -1));
                break;

            case OCI_TYPEPARAM_INOUT:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("INOUT", -1));
                break;

            default:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("", -1));

        }

        switch (data_type) {
            case OCI_TYPECODE_VARCHAR2:
            case OCI_TYPECODE_VARCHAR:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("VARCHAR2", -1));
                break;

            case OCI_TYPECODE_CHAR:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("CHAR", -1));
                break;

            case OCI_TYPECODE_CLOB:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("CLOB", -1));
                break;

            case OCI_TYPECODE_NUMBER:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("NUMBER", -1));
                break;

            case OCI_TYPECODE_DATE:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("DATE", -1));
                break;

            case OCI_TYPECODE_OBJECT:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("OBJECT", -1));
                break;

            case SQLT_CUR:
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("REF CURSOR", -1));
                break;

            default:
                Ns_Log(Warning, "Unknown Oracle Type: %d", data_type);
                Tcl_ListObjAppendElement(interp, argument, Tcl_NewStringObj("", -1));

        }

        Tcl_ListObjAppendElement(interp, argument, Tcl_NewIntObj(has_default));
        Tcl_ListObjAppendElement(interp, argObj, argument);
    }

    if (list) {
        Tcl_ListObjAppendElement(interp, list, argObj);
        Tcl_ListObjAppendElement(interp, Tcl_GetObjResult(interp), list);
    }

}
/*}}}*/

/*
 * AOLserver [ns_db] implementation.
 *
 */

/*{{{ Ns_DbDriverInit */
/* Entry point (called by AOLserver when driver loaded)

   note that this does not leave behind any structures or state outside
   of reading the configuration parameters, as well as
   initializing OCI and registering our functions
*/
NS_EXPORT Ns_ReturnCode
Ns_DbDriverInit (const char *hdriver, const char *config_path)
{
    Ns_ReturnCode ns_status;

    debug_p = Ns_ConfigBool(config_path, "Debug", DEFAULT_DEBUG);
    convert_encoding_p = Ns_ConfigBool(config_path, "ConvertEncoding", NS_FALSE);

    max_string_log_length = Ns_ConfigIntRange(config_path, "MaxStringLogLength", DEFAULT_MAX_STRING_LOG_LENGTH,
                                              100, INT_MAX);
    char_expansion = Ns_ConfigIntRange(config_path, "CharExpansion", DEFAULT_CHAR_EXPANSION, 1, 4);

    lob_buffer_size = (unsigned int)Ns_ConfigIntRange(config_path, "LobBufferSize", 16384, 1, 128000);
    Ns_Log(Notice, "%s driver LobBufferSize = %d", hdriver, lob_buffer_size);

    prefetch_rows = Ns_ConfigIntRange(config_path, "PrefetchRows", 0, 0, 1000000);
    Ns_Log(Notice, "%s driver PrefetchRows = %d", hdriver, prefetch_rows);

    prefetch_memory = Ns_ConfigIntRange(config_path, "PrefetchMemory", 0, 0, INT_MAX);
    Ns_Log(Notice, "%s driver PrefetchMemory = %d", hdriver, prefetch_memory);

    ns_ora_log(lexpos(), "entry (hdriver %p, config_path %s)", hdriver, nilp(config_path));

    ns_status = Ns_DbRegisterDriver(hdriver, ora_procs);
    if (ns_status != NS_OK) {
        error(lexpos(), "Could not register driver `%s'.",
              nilp(ora_driver_name));
        return NS_ERROR;
    }

    Ns_Log(Notice, "Loaded %s, built on %s/%s",
           ora_driver_version, __TIME__, __DATE__);

#if defined(FOR_CASSANDRACLE)
    Ns_Log(Notice,
           "    This Oracle Driver is a reduced-functionality Cassandracle driver");
#endif

    ns_ora_log(lexpos(), "driver `%s' loaded.", nilp(ora_driver_name));

    return NS_OK;
}
/*}}}*/

/*{{{ Ns_OracleInterpInit */
static int
Ns_OracleInterpInit(Tcl_Interp *interp, const void *UNUSED(dummy))
{
    Tcl_CreateObjCommand (interp, "ns_ora", OracleObjCmd,
            (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

    Tcl_CreateObjCommand (interp, "ns_oracle", OracleObjCmd,
            (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

#if defined(NS_AOLSERVER_3_PLUS)
    Tcl_CreateCommand(interp, "ns_column", ora_column_command, NULL, NULL);
    Tcl_CreateCommand(interp, "ns_table", ora_table_command, NULL, NULL);
#endif

    return NS_OK;
}
/*}}}*/

/*{{{ Ns_OracleServerInit */
static Ns_ReturnCode
Ns_OracleServerInit(char *hserver, char *hmodule, char *hdriver)
{
    ns_ora_log(lexpos(), "entry (%s, %s, %s)", nilp(hserver), nilp(hmodule),
        nilp(hdriver));

    return Ns_TclRegisterTrace(hserver, Ns_OracleInterpInit, NULL, NS_TCL_TRACE_CREATE);
}
/*}}}*/

/*{{{ Ns_OracleName */
/*----------------------------------------------------------------------
 * Ns_OracleName --
 *
 *      Return name of database driver.
 *
 *----------------------------------------------------------------------
 */
static const char *
Ns_OracleName (Ns_DbHandle *dummy)
{
    ns_ora_log(lexpos(), "entry (dummy %p)", dummy);

    return ora_driver_name;
}
/*}}}*/

/*{{{ Ns_OracleDbType */
/*----------------------------------------------------------------------
 * Ns_OracleDbType --
 *
 *      This function returns the string which identifies the database
 *      type.
 *
 *      Implements [ns_db dbtype]
 *
 *----------------------------------------------------------------------
 */
static const char *
Ns_OracleDbType (Ns_DbHandle *dummy)
{
    ns_ora_log(lexpos(), "entry (dummy %p)", dummy);

    return ora_driver_name;
}
/*}}}*/

/*{{{ Ns_OracleOpenDb */
/*----------------------------------------------------------------------
 * Ns_OracleOpenDb --
 *
 *      Opens a database connection.
 *
 *      Implements [ns_db open]
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
Ns_OracleOpenDb (Ns_DbHandle *dbh)
{
    oci_status_t oci_status;
    ora_connection_t *connection;

    ns_ora_log(lexpos(), "entry (dbh %p)", dbh);

    if (dbh == NULL) {
        error(lexpos(), "invalid args.");
        return NS_ERROR;
    }

    if (! dbh->password) {
        error(lexpos(),
              "Missing Password parameter in configuration file for pool %s.",
              dbh->poolname );
        return NS_ERROR;
    }

    if (! dbh->user) {
        error(lexpos(),
              "Missing User parameter in configuration file for pool %s.",
              dbh->poolname);
        return NS_ERROR;
    }

    connection = Ns_Malloc(sizeof *connection);

    connection->dbh = dbh;
    connection->env = NULL;
    connection->err = NULL;
    connection->srv = NULL;
    connection->svc = NULL;
    connection->auth = NULL;
    connection->stmt = NULL;
    connection->mode = autocommit;
    connection->n_columns = 0;
    connection->fetch_buffers = NULL;

    /*  AOLserver, in their database handle structure, gives us one field
     *  to store our connection structure.
     */
    dbh->connection = connection;

    if (convert_encoding_p) {
        /*
         * Value for the character set IDs. Since the client side (Tcl) is
         * always converting from and to UTF-8, we tell Oracle that the client
         * is UTF-8 and not necessarily the same as the database
         * encoding. Since the interface requires the ID to be set when
         * establishing the connection, we provide here the value hard-coded
         * (which seems common practice).
         *
         * The ID can be obtained from Oracle via the following SQL statement.
         *
         *    col nls_charset_id for 9999
         *    col value for a20
         *    select nls_charset_id(value) nls_charset_id, value from v$nls_valid_values
         *           where parameter = 'CHARACTERSET' and value like '%UTF%';
         */
        const ub2 AL32UTF8 = 873;

        oci_status = OCIEnvNlsCreate(&connection->env,
                                     OCI_THREADED|OCI_ENV_NO_MUTEX,
                                     NULL,
                                     Ns_OracleMalloc,
                                     Ns_OracleRealloc,
                                     Ns_OracleFree,
                                     0, NULL,
                                     AL32UTF8, AL32UTF8
                                     );
        if (oci_error_p(lexpos(), NULL, "OCIEnvNlsCreate", 0, oci_status)) {
            return NS_ERROR;
        }
    } else {
        oci_status = OCIEnvCreate(&connection->env,
                                  OCI_THREADED|OCI_ENV_NO_MUTEX,
                                  NULL,
                                  Ns_OracleMalloc,
                                  Ns_OracleRealloc,
                                  Ns_OracleFree,
                                  0, NULL);
        if (oci_error_p(lexpos(), NULL, "OCIEnvCreate", 0, oci_status)) {
            return NS_ERROR;
        }
    }

    /* sets connection->err */
    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & connection->err,
                                OCI_HTYPE_ERROR, 0, NULL);
    if (oci_error_p(lexpos(), dbh, "OCIHandleAlloc", 0, oci_status))
        return NS_ERROR;

    /* sets connection->srv */
    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & connection->srv,
                                OCI_HTYPE_SERVER, 0, NULL);
    if (oci_error_p(lexpos(), dbh, "OCIHandleAlloc", 0, oci_status))
        return NS_ERROR;

    /* sets connection->svc */
    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & connection->svc,
                                OCI_HTYPE_SVCCTX, 0, NULL);
    if (oci_error_p(lexpos(), dbh, "OCIHandleAlloc", 0, oci_status))
        return NS_ERROR;

    /*
     * Create association between server handle and access path (datasource;
     * a string from the configuration file).
     */
    oci_status = OCIServerAttach(connection->srv, connection->err,
                                 (const OraText *)dbh->datasource,
                                 (sb4) strlen(dbh->datasource), OCI_DEFAULT);
    if (oci_error_p(lexpos(), dbh, "OCIServerAttach", 0, oci_status))
        return NS_ERROR;

    /* tell OCI to associate the server handle with the context handle */
    oci_status = OCIAttrSet(connection->svc,
                            OCI_HTYPE_SVCCTX,
                            (void *)connection->srv,
                            0, OCI_ATTR_SERVER, connection->err);
    if (oci_error_p(lexpos(), dbh, "OCIAttrSet", 0, oci_status))
        return NS_ERROR;

    /* allocate connection->auth */
    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & connection->auth,
                                OCI_HTYPE_SESSION, 0, NULL);
    if (oci_error_p(lexpos(), dbh, "OCIHandleAlloc", 0, oci_status))
        return NS_ERROR;

    /* give OCI the username from the nsd.ini file */
    oci_status = OCIAttrSet(connection->auth,
                            OCI_HTYPE_SESSION,
                            (void *)dbh->user,
                            (ub4) strlen(dbh->user),
                            OCI_ATTR_USERNAME, connection->err);
    if (oci_error_p(lexpos(), dbh, "OCIAttrSet", 0, oci_status))
        return NS_ERROR;

    /* give OCI the password from the nsd.ini file */
    oci_status = OCIAttrSet(connection->auth,
                            OCI_HTYPE_SESSION,
                            (void *)dbh->password,
                            (ub4) strlen(dbh->password),
                            OCI_ATTR_PASSWORD, connection->err);
    if (oci_error_p(lexpos(), dbh, "OCIAttrSet", 0, oci_status))
        return NS_ERROR;

    /* the OCI docs say this "creates a user session and begins a
       user session for a given server */
    oci_status = OCISessionBegin(connection->svc,
                                 connection->err,
                                 connection->auth,
                                 OCI_CRED_RDBMS, OCI_DEFAULT);
    if (oci_error_p(lexpos(), dbh, "OCISessionBegin", 0, oci_status))
        return NS_ERROR;

    /* associate the particular authentications with a particular context */
    oci_status = OCIAttrSet(connection->svc,
                            OCI_HTYPE_SVCCTX,
                            connection->auth,
                            0, OCI_ATTR_SESSION, connection->err);
    if (oci_error_p(lexpos(), dbh, "OCIAttrSet", 0, oci_status))
        return NS_ERROR;

    ns_ora_log(lexpos(), "(dbh %p); return NS_OK;", dbh);

    dbh->connected = NS_TRUE;

    return NS_OK;
}
/*}}}*/

/*{{{ Ns_OracleCloseDb */
/*----------------------------------------------------------------------
 * Ns_OracleCloseDb --
 *
 *      Closes a database connection and clean up handle.
 *
 *      Implements [ns_db close]
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
Ns_OracleCloseDb (Ns_DbHandle *dbh)
{
    oci_status_t oci_status;
    ora_connection_t *connection;

    ns_ora_log(lexpos(), "entry (dbh %p)", dbh);

    if (dbh == NULL) {
        error(lexpos(), "invalid args.");
        return NS_ERROR;
    }

    connection = dbh->connection;
    if (!connection) {
        error(lexpos(), "no connection.");
        return NS_ERROR;
    }

    /* don't return on error; just clean up the best we can */
    oci_status = OCIServerDetach(connection->srv,
                                 connection->err, OCI_DEFAULT);
    oci_error_p(lexpos(), dbh, "OCIServerDetach", 0, oci_status);

    oci_status = OCIHandleFree(connection->svc, OCI_HTYPE_SVCCTX);
    oci_error_p(lexpos(), dbh, "OCIHandleFree", 0, oci_status);
    connection->svc = 0;

    oci_status = OCIHandleFree(connection->srv, OCI_HTYPE_SERVER);
    oci_error_p(lexpos(), dbh, "OCIHandleFree", 0, oci_status);
    connection->srv = 0;

    oci_status = OCIHandleFree(connection->err, OCI_HTYPE_ERROR);
    oci_error_p(lexpos(), dbh, "OCIHandleFree", 0, oci_status);
    connection->err = 0;

    oci_status = OCIHandleFree(connection->auth, OCI_HTYPE_SESSION);
    oci_error_p(lexpos(), dbh, "OCIHandleFree", 0, oci_status);
    connection->auth = 0;

    oci_status = OCIHandleFree(connection->env, OCI_HTYPE_ENV);
    oci_error_p(lexpos(), dbh, "OCIHandleFree", 0, oci_status);
    connection->env = 0;

    Ns_Free(connection);
    dbh->connection = NULL;
    dbh->connected = NS_FALSE;

    return NS_OK;
}
/*}}}*/

/*{{{ Ns_OracleSelect */
/*----------------------------------------------------------------------
 * Ns_OracleSelect --
 *
 *      Execute a select statement and bindrow it.
 *
 *      Implements [ns_db select]
 *
 *----------------------------------------------------------------------
 */
static Ns_Set *
Ns_OracleSelect (Ns_DbHandle *dbh, char *sql)
{
    int ns_status;

    ns_ora_log(lexpos(), "entry (dbh %p, sql %s)", dbh, nilp(sql));

    if (dbh  == NULL || sql == NULL) {
        error(lexpos(), "invalid args.");
        return 0;
    }

    ns_status = Ns_OracleExec(dbh, sql);
    if (ns_status != NS_ROWS) {
        Ns_OracleFlush(dbh);
        return 0;
    }

    return Ns_OracleBindRow(dbh);
}
/*}}}*/

/*{{{ Ns_OracleDML */
/*----------------------------------------------------------------------
 * Ns_OracleDML --
 *
 *      Execute a DML statement.
 *
 *      Implements [ns_db dml]
 *
 *----------------------------------------------------------------------
 */
static int
Ns_OracleDML (Ns_DbHandle *dbh, char *sql)
{
    int ns_status;

    ns_status = Ns_OracleExec(dbh, sql);
    if (ns_status != NS_DML) {
        Ns_OracleFlush(dbh);
        return NS_ERROR;
    }

    return NS_OK;
}
/*}}}*/

/*{{{ Ns_OracleExec */
/*----------------------------------------------------------------------
 * Ns_OracleExec --
 *
 *      Execute a query regardless of type.
 *
 *      Implements [ns_db exec]
 *
 *----------------------------------------------------------------------
 */
static int
Ns_OracleExec (Ns_DbHandle *dbh, char *sql)
{
    oci_status_t oci_status;
    ora_connection_t *connection;
    ub4 iters;
    ub2 type;

    ns_ora_log(lexpos(), "generate simple message");
    ns_ora_log(lexpos(), "entry (dbh %p, sql %s)", dbh, nilp(sql));

    if (dbh == NULL || sql == NULL) {
        error(lexpos(), "invalid args.");
        return NS_ERROR;
    }

    connection = dbh->connection;
    if (connection == 0) {
        error(lexpos(), "no connection.");
        return NS_ERROR;
    }

    /* nuke any previously executing stmt */
    Ns_OracleFlush(dbh);

    /* handle_builtins will flush the handles on an ERROR exit */

    switch (handle_builtins(dbh, sql)) {
    case NS_DML:
        /* handled */
        return NS_DML;

    case NS_ERROR:
        return NS_ERROR;

    case NS_OK:
        break;

    default:
        error(lexpos(), "internal error");
        return NS_ERROR;
    }

    /* allocate a new handle and stuff in connection->stmt */
    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & connection->stmt,
                                OCI_HTYPE_STMT, 0, NULL);
    if (oci_error_p(lexpos(), dbh, "OCIHandleAlloc", sql, oci_status)) {
        Ns_OracleFlush(dbh);
        return NS_ERROR;
    }

    /* purely a local call to "prepare statement for execution" */
    oci_status = OCIStmtPrepare(connection->stmt,
                                connection->err,
                                (const OraText *)sql,
                                (ub4) strlen(sql),
                                OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (oci_error_p(lexpos(), dbh, "OCIStmtPrepare", sql, oci_status)) {
        Ns_OracleFlush(dbh);
        return NS_ERROR;
    }

    /* check what type of statement it is, this will affect how
       many times we expect to execute it */
    oci_status = OCIAttrGet(connection->stmt,
                            OCI_HTYPE_STMT,
                            (oci_attribute_t *) & type,
                            NULL, OCI_ATTR_STMT_TYPE, connection->err);
    if (oci_error_p(lexpos(), dbh, "OCIAttrGet", sql, oci_status)) {
        /* got error asking Oracle for the statement type */
        Ns_OracleFlush(dbh);
        return NS_ERROR;
    }

    if (type == OCI_STMT_SELECT) {
        iters = 0;
        if (prefetch_rows > 0) {
            /* Set prefetch rows attr for selects. */
            oci_status = OCIAttrSet(connection->stmt,
                                    OCI_HTYPE_STMT,
                                    (dvoid *) &prefetch_rows,
                                    0,
                                    OCI_ATTR_PREFETCH_ROWS,
                                    connection->err);
            if (oci_error_p(lexpos(), dbh, "OCIAttrSet", sql, oci_status)) {
                Ns_OracleFlush(dbh);
                return NS_ERROR;
            }
        }
        if (prefetch_memory > 0) {
            /* Set prefetch memory attr for selects. */
            oci_status = OCIAttrSet(connection->stmt,
                                    OCI_HTYPE_STMT,
                                    (dvoid *) &prefetch_memory,
                                    0,
                                    OCI_ATTR_PREFETCH_MEMORY,
                                    connection->err);
            if (oci_error_p(lexpos(), dbh, "OCIAttrSet", sql, oci_status)) {
                Ns_OracleFlush(dbh);
                return NS_ERROR;
            }
        }
    } else {
        iters = 1;
    }

    /* actually go to server and execute statement */
    oci_status = OCIStmtExecute(connection->svc,
                                connection->stmt,
                                connection->err,
                                iters,
                                0, NULL, NULL,
                                (connection->mode == autocommit
                                 ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT));
    if (oci_status == OCI_ERROR) {
        oci_status_t oci_status1;
        sb4 errorcode;

        oci_status1 = OCIErrorGet(connection->err,
                                  1,
                                  NULL, &errorcode, 0, 0, OCI_HTYPE_ERROR);

        if (oci_error_p(lexpos(), dbh, "OCIErrorGet", sql, oci_status1)) {
            /* the error getter got an error; let's bail */
            Ns_OracleFlush(dbh);
            return NS_ERROR;
        } else if (oci_error_p(lexpos(), dbh, "OCIStmtExecute", sql, oci_status)) {
            /* this is where we end up for an ordinary error-producing SQL statement
               we call oci_error_p() above so that crud ends up in the log */
            Ns_OracleFlush(dbh);
            return NS_ERROR;
        }
    } else
        if (oci_error_p(lexpos(), dbh, "OCIStmtExecute", sql, oci_status))
    {
        /* we got some weird error that wasn't OCI_ERROR; we hardly ever get here */
        Ns_OracleFlush(dbh);
        return NS_ERROR;
    }

    ns_ora_log(lexpos(), "query type `%d'", type);

    if (type == OCI_STMT_SELECT)
        return NS_ROWS;
    else
        return NS_DML;
}
/*}}}*/

/*{{{ Ns_OracleBindRow */
/*----------------------------------------------------------------------
 * Ns_OracleBindRow --
 *
 *      Return a list of column names in an Ns_Set.  This is used
 *      later to fetch rows into.
 *
 *      Implements [ns_db bindrow]
 *
 *----------------------------------------------------------------------
 */
static Ns_Set *
Ns_OracleBindRow (Ns_DbHandle *dbh)
{
    oci_status_t oci_status;
    ora_connection_t *connection;
    Ns_Set *row = 0;
    int i;

    ns_ora_log(lexpos(), "entry (dbh %p)", dbh);

    if (dbh == NULL) {
        error(lexpos(), "invalid args.");
        return 0;
    }

    connection = dbh->connection;
    if (connection == 0) {
        error(lexpos(), "no connection.");
        return 0;
    }

    if (connection->stmt == 0) {
        error(lexpos(), "no active query statement executing");
        return 0;
    }

    if (connection->fetch_buffers != 0) {
        error(lexpos(), "query already bound");
        Ns_OracleFlush(dbh);
        return 0;
    }

    row = dbh->row;

    /* get number of columns returned by query; sets connection->n_columns */
    oci_status = OCIAttrGet(connection->stmt,
                            OCI_HTYPE_STMT,
                            (oci_attribute_t *) & connection->n_columns,
                            NULL, OCI_ATTR_PARAM_COUNT, connection->err);
    if (oci_error_p(lexpos(), dbh, "OCIAttrGet", 0, oci_status)) {
        Ns_OracleFlush(dbh);
        return 0;
    }

    ns_ora_log(lexpos(), "n_columns: %d", connection->n_columns);

    /* allocate N fetch buffers, this proc pulls N from connection->n_columns */
    malloc_fetch_buffers(connection);

    for (i = 0; i < connection->n_columns; i++) {
        fetch_buffer_t *fetchbuf;
        OCIParam *param;

        /* 512 is large enough because Oracle sends back table_name.column_name and
           neither right now can be larger than 30 chars */
        char name[512];
        char *name1 = NULL;
        ub4 name1_size = 0;
        const char *caseLabel;

        /* set current fetch buffer */
        fetchbuf = &connection->fetch_buffers[i];

        oci_status = OCIParamGet(connection->stmt,
                                 OCI_HTYPE_STMT,
                                 connection->err,
                                 (oci_param_t *) & param, (ub4)i + 1);
        if (oci_error_p(lexpos(), dbh, "OCIParamGet", 0, oci_status)) {
            Ns_OracleFlush(dbh);
            return 0;
        }

        oci_status = OCIAttrGet(param,
                                OCI_DTYPE_PARAM,
                                (oci_attribute_t *) & name1,
                                &name1_size, OCI_ATTR_NAME,
                                connection->err);
        if (oci_error_p(lexpos(), dbh, "OCIAttrGet", 0, oci_status)) {
            Ns_OracleFlush(dbh);
            return 0;
        }

        /* Oracle gives us back a pointer to a string that is not null-terminated
           so we copy it into our local var and add a 0 at the end */
        memcpy(name, name1, name1_size);
        name[name1_size] = 0;
        /* we downcase the column name for backward-compatibility with philg's
           AOLserver Tcl scripts written for the case-sensitive Illustra
           RDBMS.  philg was lucky in that he always used lowercase.  You might want
           to change this to leave everything all-uppercase if you're a traditional
           Oracle shop */
        downcase(name);

        ns_ora_log(lexpos(), "name %d `%s'", name1_size, name);
        Ns_SetPut(row, name, 0);

        /* get the column type */
        oci_status = OCIAttrGet(param,
                                OCI_DTYPE_PARAM,
                                (oci_attribute_t *) & fetchbuf->type,
                                NULL, OCI_ATTR_DATA_TYPE, connection->err);
        if (oci_error_p(lexpos(), dbh, "OCIAttrGet", 0, oci_status)) {
            Ns_OracleFlush(dbh);
            return 0;
        }

        /* ns_ora_log(lexpos(), "%d: column `%s' type `%d'", i. name, fetchbuf->type);*/

        switch (fetchbuf->type) {
            /* we handle LOBs in the loop below */
        case OCI_TYPECODE_CLOB:
        case OCI_TYPECODE_BLOB:
            caseLabel = "lob";
            break;

            /* RDD is Oracle's happy fun name for ROWID (18 chars long
               but if you ask Oracle the usual way, it will give you a
               number that is too small) */
        case SQLT_RDD:
            caseLabel = "rdd";
            fetchbuf->size = 18;
            fetchbuf->buf_size = fetchbuf->size + 8;
            fetchbuf->buf = Ns_Malloc(fetchbuf->buf_size);
            break;

        case SQLT_NUM:
            /* OCI reports that all NUMBER values has a size of 22, the size
               of its internal storage format for numbers. We are fetching
               all values out as strings, so we need more space. Empirically,
               it seems to return 41 characters when it does the NUMBER to STRING
               conversion. */
            caseLabel = "num";
            fetchbuf->size = 81;
            fetchbuf->buf_size = fetchbuf->size + 8;
            fetchbuf->buf = Ns_Malloc(fetchbuf->buf_size);
            break;

            /* this might work if the rest of our LONG stuff worked */
        case SQLT_LNG:
            caseLabel = "long";
            fetchbuf->buf_size = lob_buffer_size;
            fetchbuf->buf = Ns_Malloc(fetchbuf->buf_size);
            break;

        case SQLT_DAT:
        case SQLT_TIMESTAMP:
        case SQLT_TIMESTAMP_TZ:
            if (fetchbuf->type == SQLT_DAT) {
                /*
                 * date with format "YYYY-MM-DD HH24:MI:SS",
                 * 20 bytes
                 */
                caseLabel = "date";
                fetchbuf->size = 20;
            } else if (fetchbuf->type == SQLT_TIMESTAMP) {
                /*
                 * timestamp with format "YYYY-MM-DD HH24:MI:SS.FF6",
                 * 26 bytes
                 */
                caseLabel = "timestamp";
                fetchbuf->size = 26;
            } else {
                /*
                 * timestamp tz with format "YYYY-MM-DD HH24:MI:SS.FF6 TZH:TZM",
                 * 33 bytes
                 */
               caseLabel = "timestamp tz";
               fetchbuf->size = 33;
            }
            fetchbuf->buf_size = fetchbuf->size + 8;
            fetchbuf->buf = Ns_Malloc(fetchbuf->buf_size);
            break;

        default:
            caseLabel = "default";
            /* get the size */
            oci_status = OCIAttrGet(param,
                                    OCI_DTYPE_PARAM,
                                    (oci_attribute_t *) & fetchbuf->size,
                                    NULL,
                                    OCI_ATTR_DATA_SIZE, connection->err);
            if (oci_error_p(lexpos(), dbh, "OCIAttrGet", 0, oci_status)) {
                Ns_OracleFlush(dbh);
                return 0;
            }

            /*ns_ora_log(lexpos(), "column `%s' size `%d'", name, fetchbuf->size);*/

            /* this is the important part, we allocate buf to be 8 bytes
               more than Oracle says are necessary (for null
               termination) */
            fetchbuf->buf_size = fetchbuf->size + 8;

            if (fetchbuf->type == SQLT_BIN) {
                fetchbuf->buf_size = fetchbuf->size * 2 + 8;
            } else {
                fetchbuf->buf_size = fetchbuf->size + 8;
            }

            fetchbuf->buf_size *= (unsigned int)char_expansion;
            fetchbuf->buf = Ns_Malloc (fetchbuf->buf_size);

            break;
        }

        ns_ora_log(lexpos(), "%d: column `%s' type %d size %d (%s)",
                   i, name, fetchbuf->type, fetchbuf->size, caseLabel);

    }

    /* loop over the columns again; this could now be in the loop above
       but we originally did things this way to permit resizing of
       buffers

       Now we're telling Oracle to associate the buffers we just
       allocated with their respective columns */
    for (i = 0; i < connection->n_columns; i++) {
        fetch_buffer_t *fetchbuf;

        fetchbuf = &connection->fetch_buffers[i];

        switch (fetchbuf->type) {
        case OCI_TYPECODE_CLOB:
        case OCI_TYPECODE_BLOB:
            /* we allocate descriptors for CLOBs; these are essentially
               pointers.  We will not allocate any buffers for them
               until we're actually fetching data from individual
               rows. */
            oci_status = OCIDescriptorAlloc(connection->env,
                                            (oci_descriptor_t *) &
                                            fetchbuf->lob, OCI_DTYPE_LOB,
                                            0, 0);
            if (oci_error_p(lexpos(), dbh, "OCIDescriptorAlloc", 0, oci_status)) {
                Ns_OracleFlush(dbh);
                return 0;
            }

            oci_status = OCIDefineByPos(connection->stmt,
                                        &fetchbuf->def,
                                        connection->err,
                                        (ub4)i + 1,
                                        &fetchbuf->lob,
                                        -1,
                                        fetchbuf->type,
                                        &fetchbuf->is_null,
                                        0, 0, OCI_DEFAULT);
            if (oci_error_p(lexpos(), dbh, "OCIDefineByPos", 0, oci_status)) {
                Ns_OracleFlush(dbh);
                return 0;
            }
            break;

        case SQLT_LNG:
            oci_status = OCIDefineByPos(connection->stmt,
                                        &fetchbuf->def,
                                        connection->err,
                                        (ub4)i + 1,
                                        0,
                                        (sb4)SB4MAXVAL,
                                        fetchbuf->type,
                                        &fetchbuf->is_null,
                                        &fetchbuf->fetch_length,
                                        0, OCI_DYNAMIC_FETCH);

            if (oci_error_p(lexpos(), dbh, "OCIDefineByPos", 0, oci_status)) {
                Ns_OracleFlush(dbh);
                return 0;
            }

            ns_ora_log(lexpos(), "`OCIDefineDynamic ()' success");
            break;

        default:
            oci_status = OCIDefineByPos(connection->stmt,
                                        &fetchbuf->def,
                                        connection->err,
                                        (ub4)i + 1,
                                        fetchbuf->buf,
                                        (sb4)fetchbuf->buf_size,
                                        SQLT_STR,
                                        &fetchbuf->is_null,
                                        &fetchbuf->fetch_length,
                                        NULL, OCI_DEFAULT);

            if (oci_error_p(lexpos(), dbh, "OCIDefineByPos", 0, oci_status)) {
                Ns_OracleFlush(dbh);
                return 0;
            }
            break;
        }
    }

    return row;
}
/*}}}*/

/*{{{ Ns_OracleGetRow */
/*----------------------------------------------------------------------
 * Ns_OracleGetRow --
 *
 *      Fetch the next row of the result set into the row Ns_Set.
 *
 *      Implements [ns_db getrow]
 *
 *----------------------------------------------------------------------
 */
static int
Ns_OracleGetRow (Ns_DbHandle *dbh, Ns_Set *row)
{
    oci_status_t oci_status;
    ora_connection_t *connection;
    int i;
    ub4 ret_len = 0;

    ns_ora_log(lexpos(), "entry (dbh %p, row %p)", dbh, row);

    if (dbh == NULL || row == NULL) {
        error(lexpos(), "invalid args.");
        return NS_ERROR;
    }

    connection = dbh->connection;
    if (connection == 0) {
        error(lexpos(), "no connection.");
        return NS_ERROR;
    }

    if (row == 0) {
        error(lexpos(), "invalid argument, `NULL' row");
        Ns_OracleFlush(dbh);
        return NS_ERROR;
    }

    if (connection->stmt == 0) {
        error(lexpos(), "no active select");
        Ns_OracleFlush(dbh);
        return NS_ERROR;
    }

    /* fetch */
    oci_status = OCIStmtFetch(connection->stmt,
                              connection->err,
                              1, OCI_FETCH_NEXT, OCI_DEFAULT);

    if (oci_status == OCI_NEED_DATA) {
        ;
    } else if (oci_status == OCI_NO_DATA) {
        /*  We've reached beyond the last row of the select, so flush the
         *  statement and tell AOLserver that it isn't going to get
         *  anything more out of us.
         */
        ns_ora_log(lexpos(), "return NS_END_DATA;");

        if (Ns_OracleFlush(dbh) != NS_OK)
            return NS_ERROR;
        else
            return NS_END_DATA;
    } else if (oci_error_p(lexpos(), dbh, "OCIStmtFetch", 0, oci_status)) {
        /* We got some other kind of error */
        Ns_OracleFlush(dbh);
        return NS_ERROR;
    }

    /* Fetched succeeded; copy fetch buffers (one/column) into the ns_set */
    for (i = 0; i < connection->n_columns; i++) {
        fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];

        switch (fetchbuf->type) {
        case OCI_TYPECODE_CLOB:
        case OCI_TYPECODE_BLOB:

            if (fetchbuf->is_null == -1) {
                Ns_SetPutValue(row, (size_t)i, "");
            } else if (fetchbuf->is_null != 0) {
                error(lexpos(), "invalid fetch buffer is_null");
                Ns_OracleFlush(dbh);
                return NS_ERROR;
            } else {
                /* CLOB is not null, let's grab it. We use an Ns_DString
                   to do this, because when dealing with variable width
                   character sets, a single character can be many bytes long
                   (in UTF8, up to six). */
                ub4 lob_length = 0;
                Tcl_DString retval;
                ub1 *bufp;

                /* Get length of LOB, in characters for CLOBs and bytes
                   for BLOBs. */
                oci_status = OCILobGetLength(connection->svc,
                                             connection->err,
                                             fetchbuf->lob, &lob_length);
                if (oci_error_p(lexpos(), dbh, "OCILobGetLength",
                                0, oci_status)) {
                    Ns_OracleFlush(dbh);
                    return NS_ERROR;
                }

                /* Initialize the buffer we're going to use for the value. */
                bufp = (ub1 *) Ns_Malloc(lob_buffer_size);
                Tcl_DStringInit(&retval);

                /* Do the read. */
                oci_status = OCILobRead(connection->svc,
                                        connection->err,
                                        fetchbuf->lob,
                                        &lob_length,
                                        (ub4) 1,
                                        bufp,
                                        lob_buffer_size,
                                        &retval, (OCICallbackLobRead)
                                        ora_append_buf_to_dstring, (ub2) 0,
                                        (ub1) SQLCS_IMPLICIT);

                if (oci_error_p(lexpos(), dbh, "OCILobRead", 0, oci_status)) {
                    Ns_OracleFlush(dbh);
                    Tcl_DStringFree(&retval);
                    Ns_Free(bufp);
                    return NS_ERROR;
                }

                Ns_SetPutValue(row, (size_t)i, Ns_DStringValue(&retval));
                Tcl_DStringFree(&retval);
                Ns_Free(bufp);
            }
            break;

        case SQLT_LNG:
            /* this is broken for multi-part LONGs.  LONGs are being deprecated
             * by Oracle anyway, so no big loss
             *
             * Maybe fixed by davis@arsdigita.com
             */
            if (fetchbuf->is_null == -1)
                fetchbuf->buf[0] = 0;
            else if (fetchbuf->is_null != 0) {
                error(lexpos(), "invalid fetch buffer is_null");
                Ns_OracleFlush(dbh);
                return NS_ERROR;
            } else {
                fetchbuf->buf[0] = 0;
                fetchbuf->fetch_length = 0;
                ret_len = 0;

                ns_ora_log(lexpos(), "LONG start: buf_size=%d fetched=%d\n",
                    fetchbuf->buf_size, fetchbuf->fetch_length);

                do {
                    //dvoid *def;
                    ub1 inoutp;
                    ub1 piece;
                    ub4 type;
                    ub4 iterp;
                    ub4 idxp;

                    fetchbuf->fetch_length += ret_len;
                    if (fetchbuf->fetch_length > fetchbuf->buf_size / 2) {
                        fetchbuf->buf_size *= 2;
                        fetchbuf->buf =
                            ns_realloc(fetchbuf->buf, fetchbuf->buf_size);
                    }
                    ret_len = fetchbuf->buf_size - fetchbuf->fetch_length;

                    oci_status = OCIStmtGetPieceInfo(connection->stmt,
                                                     connection->err,
                                                     (dvoid **) &
                                                     fetchbuf->def, &type,
                                                     &inoutp, &iterp,
                                                     &idxp, &piece);

                    if (oci_error_p(lexpos(), dbh, "OCIStmtGetPieceInfo", 0,
                         oci_status)) {
                        Ns_OracleFlush(dbh);
                        return NS_ERROR;
                    }

                    oci_status = OCIStmtSetPieceInfo(fetchbuf->def,
                                                     OCI_HTYPE_DEFINE,
                                                     connection->err,
                                                     (void *) (fetchbuf->
                                                               buf +
                                                               fetchbuf->
                                                               fetch_length),
                                                     &ret_len, piece,
                                                     &fetchbuf->is_null,
                                                     NULL);

                    if (oci_error_p(lexpos(), dbh, "OCIStmtGetPieceInfo", 0,
                         oci_status)) {
                        Ns_OracleFlush(dbh);
                        return NS_ERROR;
                    }

                    oci_status = OCIStmtFetch(connection->stmt,
                                              connection->err,
                                              1,
                                              OCI_FETCH_NEXT, OCI_DEFAULT);

                    ns_ora_log(lexpos(),
                        "LONG: status=%d ret_len=%d buf_size=%d fetched=%d\n",
                        oci_status, ret_len, fetchbuf->buf_size,
                        fetchbuf->fetch_length);

                    if (oci_status != OCI_NEED_DATA
                        && oci_error_p(lexpos(), dbh, "OCIStmtFetch", 0,
                                       oci_status)) {
                        Ns_OracleFlush(dbh);
                        return NS_ERROR;
                    }

                    if (oci_status == OCI_NO_DATA)
                        break;

                } while (oci_status == OCI_SUCCESS_WITH_INFO ||
                         oci_status == OCI_NEED_DATA);

            }

            fetchbuf->buf[fetchbuf->fetch_length] = 0;
            ns_ora_log(lexpos(), "LONG done: status=%d buf_size=%d fetched=%d\n",
                oci_status, fetchbuf->buf_size, fetchbuf->fetch_length);

            Ns_SetPutValue(row, (size_t)i, fetchbuf->buf);

            break;

        default:
            /* add null termination and then do an ns_set put */
            if (fetchbuf->is_null == -1)
                fetchbuf->buf[0] = 0;
            else if (fetchbuf->is_null != 0) {
                error(lexpos(), "invalid fetch buffer is_null");
                Ns_OracleFlush(dbh);
                return NS_ERROR;
            } else
                fetchbuf->buf[fetchbuf->fetch_length] = 0;

            Ns_SetPutValue(row, (size_t)i, fetchbuf->buf);

            break;
        }
    }

    return NS_OK;
}
/*}}}*/

/*{{{ Ns_OracleFlush */
/*----------------------------------------------------------------------
 * Ns_OracleFlush --
 *
 *      Used to clean up after an error or after we've reached the
 *      end of a result set.  Frees fetch buffers.
 *
 *      Implements [ns_db flush]
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
Ns_OracleFlush (Ns_DbHandle *dbh)
{
    ora_connection_t *connection;
    oci_status_t      oci_status;

    ns_ora_log(lexpos(), "entry (dbh %p, row %p)", dbh, 0);

    if (dbh == NULL) {
        error(lexpos(), "invalid args, `NULL' database handle");
        return NS_ERROR;
    }

    connection = dbh->connection;

    if (connection == NULL) {
        /* Connection is closed.  That's as good as flushed to me */
        return NS_OK;
    }

    if (connection->stmt != 0) {
        oci_status = OCIHandleFree(connection->stmt, OCI_HTYPE_STMT);
        if (oci_error_p(lexpos(), dbh, "OCIHandleFree", 0, oci_status))
            return NS_ERROR;
        connection->stmt = 0;
    }

    connection->interp = NULL;

    if (connection->fetch_buffers != 0) {
        int i;

        for (i = 0; i < connection->n_columns; i++) {
            fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];

            ns_ora_log(lexpos(), "fetchbuf %d, %p, %d, %p, %p, %p", i, fetchbuf,
                fetchbuf->type, fetchbuf->lob, fetchbuf->buf,
                fetchbuf->lobs);

            if (fetchbuf->lob != 0) {
                oci_status =
                    OCIDescriptorFree(fetchbuf->lob, OCI_DTYPE_LOB);
                oci_error_p(lexpos(), dbh, "OCIDescriptorFree", 0,
                            oci_status);
                fetchbuf->lob = 0;
            }

            Ns_Free(fetchbuf->buf);
            fetchbuf->buf = NULL;
            Ns_Free(fetchbuf->array_values);
            fetchbuf->array_values = NULL;

            if (fetchbuf->lobs != 0) {
                int k;
                for (k = 0; k < (int) fetchbuf->n_rows; k++) {
                    oci_status = OCIDescriptorFree(fetchbuf->lobs[k],
                                                   OCI_DTYPE_LOB);
                    oci_error_p(lexpos(), dbh, "OCIDescriptorFree", 0,
                                oci_status);
                }
                Ns_Free(fetchbuf->lobs);
                fetchbuf->lobs = NULL;
                fetchbuf->n_rows = 0;
            }
        }

        Ns_Free(connection->fetch_buffers);
        connection->fetch_buffers = 0;
    }

    return NS_OK;
}
/*}}}*/

/*{{{ Ns_OracleResetHandle */
/*----------------------------------------------------------------------
 * Ns_OracleResetHandle --
 *
 *      Called by AOLserver when a handle is returned to the
 *      database pool.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
Ns_OracleResetHandle (Ns_DbHandle *dbh)
{
    ora_connection_t *connection;

    ns_ora_log(lexpos(), "entry (dbh %p)", dbh);

    if (dbh == NULL) {
        error(lexpos(), "invalid args.");
        return 0;
    }

    connection = dbh->connection;
    if (!connection) {
        error(lexpos(), "no connection.");
        return 0;
    }

    if (connection->mode == transaction) {
        oci_status_t oci_status;

        oci_status = OCITransRollback(connection->svc,
                                      connection->err, OCI_DEFAULT);
        if (oci_error_p(lexpos(), dbh, "OCITransRollback", 0, oci_status))
            return NS_ERROR;

        connection->mode = autocommit;
    }

    return NS_OK;
}
/*}}}*/

/*
 * Utility functions for dealing with string lists.
 *
 */

/* NaviServer includes snprintf() as ns_snprintf() in "naviserver/nsthread/error.c". */

/*{{{ string_list_elt_t */
static string_list_elt_t *
string_list_elt_new(char *string)
{
    string_list_elt_t *elt =
        (string_list_elt_t *) Ns_Malloc(sizeof(string_list_elt_t));
    elt->string = string;
    elt->next = 0;

    return elt;
}
/*}}}*/

/*{{{ string_list_len */
static int
string_list_len(string_list_elt_t * head)
{
    int i = 0;

    while (head != NULL) {
        i++;
        head = head->next;
    }

    return i;
}
/*}}}*/

/*{{{ string_list_free_list */
static void
string_list_free_list(string_list_elt_t * head)
{
    string_list_elt_t *elt;

    while (head) {
        Ns_Free(head->string);
        elt = head->next;
        Ns_Free(head);
        head = elt;
    }

}
/*}}}*/

/*{{{ parse_bind_variables  */
static string_list_elt_t *
parse_bind_variables(char *input)
{
    char *p, lastchar;
    enum { base_state, instr_state, bind_state } state;
    char bindbuf[1024], *bp = bindbuf;
    string_list_elt_t *elt, *head = 0, *tail = 0;
    int current_string_length = 0;

    for (p = input, state = base_state, lastchar = '\0'; *p != '\0';
         lastchar = *p, p++) {

        switch (state) {
        case base_state:
            if (*p == '\'') {
                state = instr_state;
                current_string_length = 0;
            } else if (*p == ':') {
                bp = bindbuf;
                state = bind_state;
            }
            break;

        case instr_state:
            if (*p == '\''
                && (lastchar != '\'' || current_string_length == 0)) {
                state = base_state;
            }
            current_string_length++;
            break;

        case bind_state:
            if (*p == '=') {
                state = base_state;
                bp = bindbuf;
            } else if (!
                       (*p == '_' || *p == '$' || *p == '#'
                        || isalnum((int) *p))) {
                *bp = '\0';
                elt = string_list_elt_new(Ns_StrDup(bindbuf));
                if (tail == 0) {
                    head = tail = elt;
                } else {
                    tail->next = elt;
                    tail = elt;
                }
                state = base_state;
                p--;
            } else {
                *bp++ = *p;
            }
            break;
        }
    }

    if (state == bind_state) {
        *bp = '\0';
        elt = string_list_elt_new(Ns_StrDup(bindbuf));
        if (tail == 0) {
            head = tail = elt;
        } else {
            tail->next = elt;
            tail = elt;
        }
    }

    return head;
}
/*}}}*/

/*{{{ downcase */
static void
downcase(char *s)
{
    for (; *s; s++)
        *s = (char)tolower(*s);
}
/*}}}*/

/*{{{ nilp */
/* nilp is misnamed to some extent; handle empty or overly long strings
 *  before printing them out to logs
 */
static const char *
nilp(const char *s)
{
    if (s == 0)
        return "[nil]";

    if ((int) strlen(s) > max_string_log_length)
        return "[too long]";

    return s;
}
/*}}}*/

/*
 * Several helper functions that get used everywhere.
 */

#ifdef FOR_CASSANDRACLE
/*{{{ allow_sql_p*/

/*
 * Because Cassandracle (http://www.arsdigita.com/free-tools/cassandracle.html)
 * runs with DBA privileges, we need to prevent anything
 * Bad from happening, whether through malicious intent or just plain
 * human sloppiness.  Selects are pretty safe, so only those are allowed
 * if FOR_CASSANDRACLE is defined, disallow any sql that does not
 * begin with "select".
 *
 */

static int allow_sql_p(Ns_DbHandle * dbh, char *sql, int display_sql_p)
{
    char *trimmedSql = sql;

    /* trim off leading white space. (the int cast is necessary for the HP) */
    while (*trimmedSql && isspace((int) *trimmedSql)) {
        trimmedSql++;
    }


    /* (damned if you do, damned if you don't.  doing a
     * strlen("select") each time here is wasteful of CPU which
     * would offend the sensibilities of half the world.  hard-coding
     * the "6" offends the other half)
     */
    if (strncasecmp(trimmedSql, "select", 6) != 0) {
        int bufsize = strlen(sql) + 4096;

        /* don't put a 20,000 byte buffer on the stack here, since this case
         * should be very rare.  allocate enough space for the sql
         * plus some to handle the other text.  (4K should be enough to
         * handle the lexpos() call)
         */
        char *buf = Ns_Malloc(bufsize);

        /* means someone is trying to do something other than a select.
         * Bad!  Very Bad!
         */
        if (display_sql_p) {
            snprintf(buf, bufsize, "%s:%d:%s: Sql Rejected: %s",
                     lexpos(), trimmedSql);
        } else {
            snprintf(buf, bufsize, "%s:%d:%s: Sql Rejected", lexpos());
        }

        Ns_Log(Error, "%s", buf);

        Ns_DbSetException(dbh, "ORA", buf);

        Ns_Free(buf);

        return NS_FALSE;
    }

    return NS_TRUE;

}                               /* allow_sql_p */
/*}}}*/
#endif

/*{{{ oci_error_p */
/* we call this after every OCI call, i.e., a couple of times during
   each fetch of a row
*/
static int
oci_error_p(const char *file, int line, const char *fn,
            Ns_DbHandle *dbh, const char *ocifn, const char *query,
            oci_status_t oci_status)
{
    ora_connection_t *connection = 0;
    ub2               offset = 0;
    sb4               errorcode = 0;

    char             *msgbuf;
    char             *buf;

    if (dbh) {
        connection = dbh->connection;
    }

    if (oci_status == OCI_SUCCESS)
        return 0;

    /* Until we get the logging situation worked out, return
     * OCI_SUCCESS_WITH_INFO as a pure success.
     */
    if (oci_status == OCI_SUCCESS_WITH_INFO)
        return 0;

    /* If the query is long, nilp will return "[too long]";
     * if null (we're not doing a query yet, e.g., could
     * be opening db), then "[nil]"
     */
    query = nilp(query);

    msgbuf = (char *) Ns_Malloc(STACK_BUFFER_SIZE * sizeof(char));
    buf = (char *) Ns_Malloc(STACK_BUFFER_SIZE * sizeof(char));
    *msgbuf = 0;

    switch (oci_status) {

        case OCI_NEED_DATA:
            snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "Error - OCI_NEED_DATA");
            break;
        case OCI_NO_DATA:
            snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "Error - OCI_NO_DATA");
            break;
        case OCI_ERROR:

            if (connection == 0)
                snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "NULL connection");
            else {
                oci_status_t oci_status1;
                char         errorbuf[1024];

                oci_status1 = OCIErrorGet(connection->err,
                                          1,
                                          NULL,
                                          &errorcode,
                                          (OraText *)errorbuf,
                                          sizeof errorbuf, OCI_HTYPE_ERROR);
                if (oci_status1) {
                    snprintf(msgbuf, STACK_BUFFER_SIZE, "%s",
                             "`OCIErrorGet ()' error");
                } else {
                    snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", errorbuf);
                }

                oci_status1 = OCIAttrGet(connection->stmt,
                                         OCI_HTYPE_STMT,
                                         &offset,
                                         NULL,
                                         OCI_ATTR_PARSE_ERROR_OFFSET,
                                         connection->err);

                if (errorcode == 1041 ||
                    errorcode == 3113 ||
                    errorcode == 12571 ||
                    errorcode == 28 ||
                    errorcode == 1012 ||
                    errorcode == 24324) {

                    /* 3113 is 'end-of-file on communications channel', which
                     *      happens if the oracle process dies
                     * 12571 is TNS:packet writer failure, which also happens if
                     *      the oracle process dies
                     * 1041 is the dreaded "hostdef extension doesn't exist error,
                     *      which means the db handle is screwed and can't be used
                     *      for anything else.
                     *
                     * In either case, close and re-open the handle to clear the
                     * error condition
                     */
                    Ns_OracleFlush(dbh);
                    Ns_OracleCloseDb(dbh);
               } else if (errorcode == 20 || errorcode == 1034) {
                    /* ora-00020 means 'maximum number of processes exceeded.
                     * ora-01034 means 'oracle not available'.
                     *           we want to make sure the oracleSID process
                     *           goes away so we don't make the problem worse
                     */
                    Ns_OracleCloseDb(dbh);
                } else if (oci_status1) {
                    Ns_Log(Warning, "nsoracle: Unhandled error status %d after OCIAttrGet()",errorcode);
                }
            }
            break;
        case OCI_INVALID_HANDLE:
            snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "Error - OCI_INVALID_HANDLE");
            break;
        case OCI_STILL_EXECUTING:
            snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "Error - OCI_STILL_EXECUTING");
            break;
        case OCI_CONTINUE:
            snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "Error - OCI_CONTINUE");
            break;
    }

    if (((errorcode == 900) || (offset > 0)) && (strlen(query) >= offset)) {
        /* ora-00900 is invalid SQL statement
         *           it seems to be the msg most likely to be a parse
         *           error that sets offset to 0
         */
        int len;
        len = snprintf(buf, STACK_BUFFER_SIZE,
                       "%s:%d:%s: error in `%s ()': %s\nSQL: ",
                       file, line, fn, ocifn, msgbuf);
        if (offset > 0)
            len += snprintf(buf + len, STACK_BUFFER_SIZE - len, "%.*s",
                            offset - 1, query);

        snprintf(buf + len, STACK_BUFFER_SIZE - len, " !>>>!%s",
                 query + offset);
    } else {
        snprintf(buf, STACK_BUFFER_SIZE,
                 "%s:%d:%s: error in `%s ()': %s\nSQL: %s",
                 file, line, fn, ocifn, msgbuf, query);
    }

    Ns_Log(Error, "%s", buf);

    if (dbh != NULL) {
        char exceptbuf[EXCEPTION_CODE_SIZE + 1];

        /* We need to call this so that AOLserver will print out the relevant
         * error on pages served to browsers where ClientDebug is set.
         */
        snprintf(exceptbuf, EXCEPTION_CODE_SIZE, "%d", (int) errorcode);
        Ns_DbSetException(dbh, exceptbuf, buf);
    }

    Ns_Free(msgbuf);
    Ns_Free(buf);

    return 1;
}
/*}}}*/

/*{{{ tcl_error_p */
/* tcl_error_p is only used for ns_ora and potentially other new Tcl commands
   does not log the error and does not Ns_DbSetException but instead just
   tells the Tcl interpreter about it
 */
static int
tcl_error_p(const char *file, int line, const char *fn,
            Tcl_Interp *interp,
            Ns_DbHandle *dbh, const char *ocifn, const char *query,
            oci_status_t oci_status)
{
    char *msgbuf;
    char *buf;
    ora_connection_t *connection = 0;
    ub2 offset = 0;
    sb4 errorcode = 0;

    if (dbh)
        connection = dbh->connection;

    /* success */
    if (oci_status == OCI_SUCCESS)
        return 0;

    query = nilp(query);
    msgbuf = (char *) Ns_Malloc(STACK_BUFFER_SIZE * sizeof(char));
    buf = (char *) Ns_Malloc(STACK_BUFFER_SIZE * sizeof(char));
    *msgbuf = 0;

    switch (oci_status) {
    case OCI_SUCCESS_WITH_INFO:
        snprintf(msgbuf, STACK_BUFFER_SIZE,"%s",
                 "Error - OCI_SUCCESS_WITH_INFO");
        break;
    case OCI_NEED_DATA:
        snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "Error - OCI_NEED_DATA");
        break;
    case OCI_NO_DATA:
        snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "Error - OCI_NO_DATA");
        break;
    case OCI_ERROR:
        if (connection == 0)
            snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "NULL connection");
        else {
            char errorbuf[512];
            oci_status_t oci_status1;

            oci_status1 = OCIErrorGet(connection->err,
                                      1,
                                      NULL,
                                      &errorcode,
                                      (OraText *)errorbuf,
                                      sizeof errorbuf, OCI_HTYPE_ERROR);

            if (oci_status1) {
                snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "`OCIErrorGet ()' error");
            } else {
                snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", errorbuf);
            }

            oci_status1 = OCIAttrGet(connection->stmt,
                                     OCI_HTYPE_STMT,
                                     &offset,
                                     NULL,
                                     OCI_ATTR_PARSE_ERROR_OFFSET,
                                     connection->err);

            if (errorcode == 1041 || errorcode == 3113
                || errorcode == 12571) {
                /* 3113 is 'end-of-file on communications channel', which
                 *      happens if the oracle process dies
                 * 12571 is TNS:packet writer failure, which also happens if
                 *      the oracle process dies
                 * 1041 is the dreaded "hostdef extension doesn't exist error,
                 *      which means the db handle is screwed and can't be used
                 *      for anything else.
                 * In either case, close and re-open the handle to clear the
                 * error condition
                 */
                Ns_OracleFlush(dbh);
                Ns_OracleCloseDb(dbh);
                Ns_OracleOpenDb(dbh);
            } else if (oci_status1) {
                Ns_Log(Warning, "nsoracle: Unhandled error status %d after OCIAttrGet()",errorcode);
            }
        }
        break;
    case OCI_INVALID_HANDLE:
        snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "Error - OCI_INVALID_HANDLE");
        break;
    case OCI_STILL_EXECUTING:
        snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "Error - OCI_STILL_EXECUTING");
        break;
    case OCI_CONTINUE:
        snprintf(msgbuf, STACK_BUFFER_SIZE, "%s", "Error - OCI_CONTINUE");
        break;
    }

    snprintf(buf, STACK_BUFFER_SIZE,
             "%s:%d:%s: error in `%s ()': %s\nSQL: %s", file, line, fn,
             ocifn, msgbuf, query);

    Ns_Log(Error, "SQL(): %s", buf);

    Tcl_AppendResult(interp, buf, (char*)0L);

    /* error */
    Ns_Free(msgbuf);
    Ns_Free(buf);

    return 1;
}
/*}}}*/

/*{{{ error */
/* For logging errors that come from C code rather than
 * Oracle unhappiness.
 */
static void
error(const char *file, int line, const char *fn, const char *fmt, ...)
{
    char *buf1;
    char *buf;
    va_list ap;

    buf1 = (char *) Ns_Malloc(STACK_BUFFER_SIZE * sizeof(char));
    buf = (char *) Ns_Malloc(STACK_BUFFER_SIZE * sizeof(char));

    va_start(ap, fmt);
    vsprintf(buf1, fmt, ap);
    va_end(ap);

    snprintf(buf, STACK_BUFFER_SIZE, "%s:%d:%s: %s", file, line, fn, buf1);

    Ns_Log(Error, "%s", buf);

    Ns_Free(buf1);
    Ns_Free(buf);
}
/*}}}*/

/*{{{ log */
/*  For optional logging of all kinds of random stuff, turn on
 *  debug in the [ns/db/driver/drivername] section of your nsd.ini
 */
static void
ns_ora_log(const char *file, int line, const char *fn, const char *fmt, ...)
{
    char   *buf, *buf1;
    va_list ap;

    if (!debug_p) {
        return;
    }

    buf1 = (char *) Ns_Malloc(STACK_BUFFER_SIZE * sizeof(char));
    buf = (char *) Ns_Malloc(STACK_BUFFER_SIZE * sizeof(char));

    va_start(ap, fmt);
    vsprintf(buf1, fmt, ap);
    va_end(ap);

    snprintf(buf, STACK_BUFFER_SIZE, "%s:%d:%s: %s", file, line, fn, buf1);

    Ns_Log(Notice, "%s", buf);

    Ns_Free(buf1);
    Ns_Free(buf);
}
/*}}}*/

/*{{{ malloc_fetch_buffers*/
/*
 * malloc_fetch_buffers allocates the fetch_buffers array in the
 * specified connection.  connection->n_columns must be set to the
 * correct number before calling this function.
 */
static void
malloc_fetch_buffers(ora_connection_t * connection)
{
    int i;

    connection->fetch_buffers =
        Ns_Malloc((size_t)connection->n_columns *
                  sizeof *connection->fetch_buffers);

    for (i = 0; i < connection->n_columns; i++) {
        fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];

        fetchbuf->connection = connection;

        fetchbuf->type = 0;
        fetchbuf->lob = NULL;
        fetchbuf->bind = NULL;
        fetchbuf->def = NULL;
        fetchbuf->size = 0;
        fetchbuf->buf_size = 0;
        fetchbuf->buf = NULL;
        fetchbuf->stmt = NULL;
        fetchbuf->array_count = 0;
        fetchbuf->array_values = NULL;
        fetchbuf->is_null = 0;
        fetchbuf->fetch_length = 0;
        fetchbuf->piecewise_fetch_length = 0;
        fetchbuf->inout = 0;
        fetchbuf->name = NULL;

        fetchbuf->lobs = NULL;
        fetchbuf->is_lob = 0;
        fetchbuf->n_rows = 0;
    }

}
/*}}}*/

/*{{{ free_fetch_buffers*/
/*
 * free_fetch_buffers frees the fetch_buffers array in the specified
 * connection.  connection->n_columns must have the same value as it
 * did when malloc_fetch_buffers was called.  The non-NULL
 * dynamically-allocated components of each fetchbuf will also be freed.
 */
static void
free_fetch_buffers(ora_connection_t * connection)
{
    if (connection != NULL && connection->fetch_buffers != NULL) {
        Ns_DbHandle *dbh = connection->dbh;
        int i;
        unsigned int j;
        oci_status_t oci_status;

        for (i = 0; i < connection->n_columns; i++) {
            fetch_buffer_t *fetchbuf = &connection->fetch_buffers[i];

            if (fetchbuf->lob != NULL) {
                oci_status =
                    OCIDescriptorFree(fetchbuf->lob, OCI_DTYPE_LOB);
                oci_error_p(lexpos(), dbh, "OCIDescriptorFree", 0,
                            oci_status);
                fetchbuf->lob = NULL;
            }

            /*
             * fetchbuf->bind is automatically deallocated when its
             * statement is deallocated.
             *
             * Same for fetchbuf->def, I believe, though the manual
             * doesn't say.
             */

            if (fetchbuf->buf != NULL) {
                Ns_Free(fetchbuf->buf);
                fetchbuf->buf = NULL;
                fetchbuf->buf_size = 0;
            }

            if (fetchbuf->array_values != NULL) {
                /* allocated from Tcl_SplitList so Tcl_Free it */
                Tcl_Free((char *) fetchbuf->array_values);
                fetchbuf->array_values = NULL;
                fetchbuf->array_count = 0;
            }

            if (fetchbuf->lobs != 0) {
                for (j = 0; j < fetchbuf->n_rows; j++) {
                    oci_status = OCIDescriptorFree(fetchbuf->lobs[j],
                                                   OCI_DTYPE_LOB);
                    oci_error_p(lexpos(), dbh, "OCIDescriptorFree", 0,
                                oci_status);
                }
                Ns_Free(fetchbuf->lobs);
                fetchbuf->lobs = NULL;
                fetchbuf->n_rows = 0;
            }
        }

        Ns_Free(connection->fetch_buffers);
        connection->fetch_buffers = NULL;
    }
}
/*}}}*/

/*{{{ handle_builtins*/

/* this gets called on every query or DML.  Usually, it will
   return NS_OK ("I did nothing").  If the SQL is one of our special
   cases, e.g., "begin transaction", that aren't supposed to go through
   to Oracle, we handle it and return NS_DML ("I handled it and nobody
   else has do anything").

   return NS_ERROR on error
*/
static int
handle_builtins(Ns_DbHandle * dbh, char *sql)
{
    oci_status_t oci_status;
    ora_connection_t *connection;

    ns_ora_log(lexpos(), "entry (dbh %p, sql %s)", dbh, nilp(sql));

    /* args should be correct */
    connection = dbh->connection;

    if (!strcasecmp(sql, "begin transaction")) {
        ns_ora_log(lexpos(), "builtin `begin transaction`");

        connection->mode = transaction;

        return NS_DML;

    } else if (!strcasecmp(sql, "end transaction")) {
        ns_ora_log(lexpos(), "builtin `end transaction`");

        oci_status = OCITransCommit(connection->svc,
                                    connection->err, OCI_DEFAULT);
        if (oci_error_p(lexpos(), dbh, "OCITransCommit", sql, oci_status)) {
            Ns_OracleFlush(dbh);
            return NS_ERROR;
        }

        connection->mode = autocommit;
        return NS_DML;

    } else if (!strcasecmp(sql, "abort transaction")) {
        ns_ora_log(lexpos(), "builtin `abort transaction`");

        oci_status = OCITransRollback(connection->svc,
                                      connection->err, OCI_DEFAULT);
        if (oci_error_p(lexpos(), dbh, "OCITransRollback", sql, oci_status)) {
            Ns_OracleFlush(dbh);
            return NS_ERROR;
        }

        connection->mode = autocommit;
        return NS_DML;
    }

    if (!allow_sql_p(dbh, sql, NS_FALSE)) {
        Ns_OracleFlush(dbh);
        return NS_ERROR;
    }

    /* not handled */
    return NS_OK;
}
/*}}}*/

/*{{{ ora_append_buf_to_dstring*/
/* Callback function for LOB case in ora_get_row. */
static sb4
ora_append_buf_to_dstring(dvoid * ctxp, const dvoid *bufp, ub4 len,
                          ub1 piece)
{
    Tcl_DString *retval = (Tcl_DString *) ctxp;

    switch (piece) {
        case OCI_LAST_PIECE:
        case OCI_FIRST_PIECE:
        case OCI_NEXT_PIECE:
            Tcl_DStringAppend(retval, (char *) bufp, (TCL_SIZE_T)len);
            return OCI_CONTINUE;

        default:
            return OCI_ERROR;
    }
}
/*}}}*/

/*{{{ no_data*/

/*  This is a function that we register as a callback with Oracle for
 *  DML statements that do RETURNING FOOBAR INTO ... (this was
 *  necessitated by the clob_dml statement which was necessitated by
 *  Oracle's stupid SQL parser that can't handle string literals longer
 *  than 4000 chars)
 */

static sb4
no_data(dvoid *UNUSED(ctxp), OCIBind *UNUSED(bindp),
        ub4 UNUSED(iter), ub4 UNUSED(index), dvoid **bufpp, ub4 *alenpp, ub1 *piecep,
        dvoid **indpp)
{
    ns_ora_log(lexpos(), "entry");

    *bufpp = (dvoid *) 0;
    *alenpp = 0;
    null_ind = -1;
    *indpp = (dvoid *) & null_ind;
    *piecep = OCI_ONE_PIECE;

    return OCI_CONTINUE;
}
/*}}}*/

/*{{{ list_element_put_data*/
/* For use by OCIBindDynamic: returns the iter'th element (0-relative)
   of the context pointer taken as an array of strings (char**). */
static sb4
list_element_put_data(dvoid * ictxp,
                      OCIBind * UNUSED(bindp),
                      ub4 iter,
                      ub4 UNUSED(index),
                      dvoid ** bufpp,
                      ub4 * alenp, ub1 * piecep, dvoid ** indpp)
{
    fetch_buffer_t *fetchbuf = ictxp;
    const char **elements = fetchbuf->array_values;

    *bufpp = (dvoid *)elements[iter];
    *alenp = (ub4) strlen(elements[iter]);
    *piecep = OCI_ONE_PIECE;
    *indpp = NULL;

    return OCI_CONTINUE;
}
/*}}}*/

/*{{{ get_data*/
/* another callback to register with Oracle */
static sb4
get_data(dvoid * ctxp, OCIBind * bindp,
         ub4 iter, ub4 index, dvoid ** bufpp, ub4 ** alenp, ub1 * piecep,
         dvoid ** indpp, ub2 ** rcodepp)
{
    Ns_DbHandle      *dbh;
    ora_connection_t *connection;
    fetch_buffer_t   *buf;

    ns_ora_log(lexpos(), "entry (dbh %p; iter %d, index %d)", ctxp, iter, index);

    if (iter != 0) {
        error(lexpos(), "iter != 0");
        return NS_ERROR;
    }

    buf = ctxp;
    connection = buf->connection;
    dbh = connection->dbh;

    if (buf->lobs == 0) {
        oci_status_t oci_status;
        int i;

        oci_status = OCIAttrGet(bindp,
                                OCI_HTYPE_BIND,
                                (oci_attribute_t *) & buf->n_rows,
                                NULL,
                                OCI_ATTR_ROWS_RETURNED, connection->err);
        if (oci_error_p(lexpos(), dbh, "OCIAttrGet", 0, oci_status))
            return NS_ERROR;

        ns_ora_log(lexpos(), "n_rows %d", buf->n_rows);

        buf->lobs = Ns_Malloc(buf->n_rows * sizeof *buf->lobs);

        for (i = 0; i < (int) buf->n_rows; i++)
            buf->lobs[i] = 0;

        for (i = 0; i < (int) buf->n_rows; i++) {
            oci_status = OCIDescriptorAlloc(connection->env,
                                            (oci_descriptor_t *) & buf->
                                            lobs[i], OCI_DTYPE_LOB, 0, 0);
            if (oci_error_p(lexpos(), dbh, "OCIDescriptorAlloc", 0, oci_status))
                return NS_ERROR;
        }
    }

    *bufpp = (dvoid *) buf->lobs[index];

    *alenp = &rl;
    null_ind = -1;
    *indpp = (dvoid *) & null_ind;
    *piecep = OCI_ONE_PIECE;
    *rcodepp = &rc;

    return OCI_CONTINUE;
}
/*}}}*/

/*{{{ stream_read_lob*/
/* read a file from the operating system and then stuff it into the lob
   This was cargo-culted from an example in the OCI programmer's
   guide.
 */
static int
stream_read_lob(Tcl_Interp * interp, Ns_DbHandle * dbh, int UNUSED(rowind),
                OCILobLocator * lobl, const char *path,
                ora_connection_t * connection)
{
    ub4 offset = 1;
    ub4 loblen = 0;
    ub1 *bufp = NULL;
    ub4 amtp;
    ub1 piece;
    size_t nbytes;
    ub4 remainder;
    off_t filelen = 0;
#ifdef WIN32
    int readlen;
#else
    ssize_t readlen;
#endif
    int status = NS_ERROR, fd;
    oci_status_t oci_status = OCI_SUCCESS;
    struct stat statbuf;

    fd = open(path, O_RDONLY | EXTRA_OPEN_FLAGS);

    if (fd == -1) {
        Ns_Log(Error, "%s:%d:%s Error opening file %s: %d(%s)",
               lexpos(), path, errno, strerror(errno));
        Tcl_AppendResult(interp, "can't open file ", path,
                         " for reading. ", "received error ",
                         strerror(errno), (char*)0L);
        goto bailout;
    }

    if (stat(path, &statbuf) == -1) {
        Ns_Log(Error, "%s:%d:%s Error statting %s: %d(%s)",
               lexpos(), path, errno, strerror(errno));
        Tcl_AppendResult(interp, "can't stat ", path, ". ",
                         "received error ", strerror(errno), (char*)0L);
        goto bailout;
    }
    filelen = statbuf.st_size;

    remainder = amtp = (ub4)filelen;

    ns_ora_log(lexpos(), "to do streamed write lob, amount = %d", (int) filelen);

    oci_status =
        OCILobGetLength(connection->svc, connection->err, lobl, &loblen);

    if (tcl_error_p(lexpos(), interp, dbh, "OCILobGetLength", 0, oci_status))
        goto bailout;


    ns_ora_log(lexpos(), "before stream write, lob length is %d", (int) loblen);

    if (filelen > lob_buffer_size)
        nbytes = lob_buffer_size;
    else
        nbytes = (size_t)filelen;

    bufp = (ub1 *) Ns_Malloc(lob_buffer_size);
    readlen = read(fd, bufp, nbytes);

    if (readlen < 0) {
        Ns_Log(Error, "%s:%d:%s Error reading file %s: %d(%s)",
               lexpos(), path, errno, strerror(errno));
        Tcl_AppendResult(interp, "can't read ", path,
                         " received error ", strerror(errno), (char*)0L);
        goto bailout;
    }

    remainder -= readlen;

    if (remainder == 0) {       /* exactly one piece in the file */
        if (readlen > 0) {      /* if no bytes, bypass the LobWrite to insert a NULL */
            ns_ora_log(lexpos(), "only one piece, no need for stream write");
            oci_status = OCILobWrite(connection->svc,
                                     connection->err,
                                     lobl,
                                     &amtp,
                                     offset,
                                     bufp,
                                     (ub4)readlen,
                                     OCI_ONE_PIECE, 0, 0, 0,
                                     SQLCS_IMPLICIT);
            if (tcl_error_p(lexpos(), interp, dbh, "OCILobWrite", 0, oci_status)) {
                goto bailout;
            }
        }
    } else {                    /* more than one piece */

        oci_status = OCILobWrite(connection->svc,
                                 connection->err,
                                 lobl,
                                 &amtp,
                                 offset,
                                 bufp,
                                 lob_buffer_size,
                                 OCI_FIRST_PIECE, 0, 0, 0, SQLCS_IMPLICIT);

        if (oci_status != OCI_NEED_DATA
            && tcl_error_p(lexpos(), interp, dbh, "OCILobWrite", 0,
                           oci_status)) {
            goto bailout;
        }


        piece = OCI_NEXT_PIECE;

        do {
            if (remainder > lob_buffer_size)
                nbytes = lob_buffer_size;
            else {
                nbytes = remainder;
                piece = OCI_LAST_PIECE;
            }

            readlen = read(fd, bufp, nbytes);

            if (readlen < 0) {
                Ns_Log(Error, "%s:%d:%s Error reading file %s: %d(%s)",
                       lexpos(), path, errno, strerror(errno));
                Tcl_AppendResult(interp, "can't read ", path,
                                 " received error ", strerror(errno),
                                 (char*)0L);
                piece = OCI_LAST_PIECE;
            }

            oci_status = OCILobWrite(connection->svc,
                                     connection->err,
                                     lobl,
                                     &amtp,
                                     offset,
                                     bufp,
                                     (ub4)readlen,
                                     piece, 0, 0, 0, SQLCS_IMPLICIT);
            if (oci_status != OCI_NEED_DATA
                && tcl_error_p(lexpos(), interp, dbh, "OCILobWrite", 0,
                               oci_status)) {
                goto bailout;
            }
            remainder -= readlen;

        } while (oci_status == OCI_NEED_DATA && remainder > 0);
    }

    if (tcl_error_p(lexpos(), interp, dbh, "OCILobWrite", 0, oci_status)) {
        goto bailout;
    }

    status = NS_OK;

  bailout:

    if (bufp)
        Ns_Free(bufp);
    close(fd);

    if (status != NS_OK && connection->mode == transaction) {
        ns_ora_log(lexpos(), "error writing lob.  rolling back transaction");

        oci_status = OCITransRollback(connection->svc,
                                      connection->err, OCI_DEFAULT);
        tcl_error_p(lexpos(), interp, dbh, "OCITransRollback", 0,
                    oci_status);
    }

    return status;
}
/*}}}*/

/*{{{ stream_actually_write*/
static ssize_t
stream_actually_write(int fd, Ns_Conn * conn, void *bufp, size_t length,
                      int to_conn_p)
{
    ssize_t bytes_written = 0;

    ns_ora_log(lexpos(), "entry (%d, %d, %d)", fd, length, to_conn_p);

    if (to_conn_p) {
        struct iovec sbuf;
        Ns_ReturnCode status;

        sbuf.iov_base = bufp;
        sbuf.iov_len = length;

        if ((conn->flags & NS_CONN_WRITE_ENCODED) == 0u) {
            status = Ns_ConnWriteVData(conn, &sbuf, 1, NS_CONN_STREAM);
        } else {
            status = Ns_ConnWriteVChars(conn, &sbuf, 1, NS_CONN_STREAM);
        }

        if (status == NS_OK) {
            bytes_written = (ssize_t)length;
        } else {
            bytes_written = 0;
        }
    } else {
        bytes_written = write(fd, bufp, length);
    }

    ns_ora_log(lexpos(), "exit (%d, %d, %d)", bytes_written, fd, length,
        to_conn_p);

    return bytes_written;
}
/*}}}*/

/*{{{ stream_write_lob*/
/* snarf lobs using stream mode from Oracle into local buffers, then
   write them to the given file (replacing the file if it exists) or
   out to the connection.
   This was cargo-culted from an example in the OCI programmer's
   guide.
*/
static int
stream_write_lob(Tcl_Interp * interp, Ns_DbHandle * dbh, int UNUSED(rowind),
                 OCILobLocator * lobl, const char *path, int to_conn_p,
                 OCISvcCtx * svchp, OCIError * errhp)
{
    ub4 offset = 1;
    ub4 loblen = 0;
    ub1 *bufp = NULL;
    ub4 amtp = 0;
    ub4 piece = 0;
    ub4 remainder;              /* the number of bytes for the last piece */
    int fd = 0;
    ssize_t bytes_to_write, bytes_written;
    int status = STREAM_WRITE_LOB_ERROR;
    oci_status_t oci_status;
    Ns_Conn *conn = NULL;

    if (path == NULL) {
        path = "to connection";
    }

    ns_ora_log(lexpos(), "entry (path %s)", path);

    if (to_conn_p) {
        conn = Ns_TclGetConn(interp);

        /* this Shouldn't Happen, but spew an error just in case */
        if (conn == NULL) {
            Ns_Log(Error, "%s:%d:%s: No AOLserver conn available",
                   lexpos());
            Tcl_AppendResult(interp, "No AOLserver conn available", (char*)0L);
            goto bailout;
        }
    } else {
        fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | EXTRA_OPEN_FLAGS,
                  0600);

        if (fd < 0) {
            Ns_Log(Error,
                   "%s:%d:%s: can't open %s for writing. error %d(%s)",
                   lexpos(), path, errno, strerror(errno));
            Tcl_AppendResult(interp, "can't open file ", path,
                             " for writing. ", "received error ",
                             strerror(errno), (char*)0L);
            goto bailout;
        }
    }

    oci_status = OCILobGetLength(svchp, errhp, lobl, &loblen);
    if (tcl_error_p(lexpos(), interp, dbh, "OCILobGetLength", path, oci_status))
        goto bailout;

    amtp = loblen;

    ns_ora_log(lexpos(), "loblen %d", loblen);

    bufp = (ub1 *) Ns_Malloc(lob_buffer_size);
    memset((void *) bufp, (int) '\0', (size_t) lob_buffer_size);

    oci_status = OCILobRead(svchp,
                            errhp,
                            lobl,
                            &amtp,
                            offset,
                            bufp,
                            (loblen <
                             lob_buffer_size ? loblen : lob_buffer_size),
                            0, 0, 0, SQLCS_IMPLICIT);

    switch (oci_status) {
    case OCI_SUCCESS:          /* only one piece */
        ns_ora_log(lexpos(), "stream read %d'th piece\n", (int) (++piece));

        bytes_written =
            stream_actually_write(fd, conn, bufp, loblen, to_conn_p);

        if (bytes_written != (int) loblen) {
            if (errno == EPIPE) {
                status = STREAM_WRITE_LOB_PIPE;
                goto bailout;
            }
            if (bytes_written < 0) {
                Ns_Log(Error, "%s:%d:%s error writing %s.  error %d(%s)",
                       lexpos(), path, errno, strerror(errno));
                Tcl_AppendResult(interp, "can't write ", path,
                                 " received error ", strerror(errno),
                                 (char*)0L);
                goto bailout;
            } else {
                Ns_Log(Error,
                       "%s:%d:%s error writing %s.  incomplete write of %ld out of %d",
                       lexpos(), path, bytes_written, loblen);
                Tcl_AppendResult(interp, "can't write ", path,
                                 " received error ", strerror(errno),
                                 (char*)0L);
                goto bailout;
            }
        }
        break;

    case OCI_ERROR:
        break;

    case OCI_NEED_DATA:        /* there are 2 or more pieces */

        remainder = loblen;
        /* a full buffer to write */
        bytes_written =
            stream_actually_write(fd, conn, bufp, lob_buffer_size,
                                  to_conn_p);

        if (bytes_written != lob_buffer_size) {
            if (errno == EPIPE) {
                status = STREAM_WRITE_LOB_PIPE;
                goto bailout;
            }
            if (bytes_written < 0) {
                Ns_Log(Error, "%s:%d:%s error writing %s.  error %d(%s)",
                       lexpos(), path, errno, strerror(errno));
                Tcl_AppendResult(interp, "can't write ", path,
                                 " received error ", strerror(errno),
                                 (char*)0L);
                goto bailout;
            } else {
                Ns_Log(Error,
                       "%s:%d:%s error writing %s.  incomplete write of %ld out of %d",
                       lexpos(), path, bytes_written, lob_buffer_size);
                Tcl_AppendResult(interp, "can't write ", path,
                                 " received error ", strerror(errno),
                                 (char*)0L);
                goto bailout;
            }
        }

        do {
            memset(bufp, '\0', lob_buffer_size);
            amtp = 0;

            remainder -= lob_buffer_size;

            oci_status = OCILobRead(svchp,
                                    errhp,
                                    lobl,
                                    &amtp,
                                    offset,
                                    bufp,
                                    lob_buffer_size,
                                    0, 0, 0, SQLCS_IMPLICIT);
            if (oci_status != OCI_NEED_DATA
                && tcl_error_p(lexpos(), interp, dbh, "OCILobRead", 0,
                               oci_status)) {
                goto bailout;
            }


            /* the amount read returned is undefined for FIRST, NEXT pieces */
            ns_ora_log(lexpos(), "stream read %d'th piece, atmp = %d",
                (int) (++piece), (int) amtp);

            if (remainder < lob_buffer_size) {  /* last piece not a full buffer piece */
                bytes_to_write = remainder;
            } else {
                bytes_to_write = lob_buffer_size;
            }

            bytes_written =
                stream_actually_write(fd, conn, bufp, (size_t)bytes_to_write,
                                      to_conn_p);

            if (bytes_written != bytes_to_write) {
                if (errno == EPIPE) {
                    /* broken pipe means the user hit the stop button.
                     * if that's the case, lie and say we've completed
                     * successfully so we don't cause false-positive errors
                     * in the server.log
                     * photo.net ticket # 5901
                     */
                    status = STREAM_WRITE_LOB_PIPE;
                } else {
                    if (bytes_written < 0) {
                        Ns_Log(Error,
                               "%s:%d:%s error writing %s.  error %d(%s)",
                               lexpos(), path, errno, strerror(errno));
                        Tcl_AppendResult(interp, "can't write ", path,
                                         " for writing. ",
                                         " received error ",
                                         strerror(errno), (char*)0L);
                    } else {
                        Ns_Log(Error,
                               "%s:%d:%s error writing %s.  incomplete write of %ld out of %ld",
                               lexpos(), path, bytes_written,
                               bytes_to_write);
                        Tcl_AppendResult(interp, "can't write ", path,
                                         " for writing. ",
                                         " received error ",
                                         strerror(errno), (char*)0L);
                    }
                }
                goto bailout;
            }
        } while (oci_status == OCI_NEED_DATA);
        break;

    default:
        Ns_Log(Error, "%s:%d:%s: Unexpected error from OCILobRead (%d)",
               lexpos(), oci_status);
        goto bailout;
        break;
    }

    status = STREAM_WRITE_LOB_OK;

  bailout:
    if (bufp)
        Ns_Free(bufp);

    if (!to_conn_p) {
        close(fd);
    }

    return status;
}
/*}}}*/

/*
 * AOLserver 3 Plus (pre-3.x) implementation
 */

#if defined(NS_AOLSERVER_3_PLUS)

/*{{{ ora_get_table_info*/
/* this is for the AOLserver extended table info stuff.  Mostly it is
   useful for the /NS/Db pages
*/
static Ns_DbTableInfo *
ora_get_table_info(Ns_DbHandle * dbh, const char *table) {
#define SQL_BUFFER_SIZE 1024
    oci_status_t oci_status;
    ora_connection_t *connection;
    char sql[SQL_BUFFER_SIZE];
    OCIStmt *stmt;
    Ns_DbTableInfo *tinfo;
    int i;
    sb4 n_columns;

    ns_ora_log(lexpos(), "entry (dbh %p, table %s)", dbh, nilp(table));

    if (dbh == NULL || table == NULL) {
        error(lexpos(), "invalid args.");
        return 0;
    }

    connection = dbh->connection;
    if (!connection) {
        error(lexpos(), "no connection.");
        return 0;
    }

    snprintf(sql, SQL_BUFFER_SIZE, "select * from %s", table);

    tinfo = Ns_DbNewTableInfo(table);

    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & stmt,
                                OCI_HTYPE_STMT, 0, NULL);
    if (oci_error_p(lexpos(), dbh, "OCIHandleAlloc", sql, oci_status))
        return 0;

    oci_status = OCIStmtPrepare(stmt,
                                connection->err,
                                (const OraText *)sql,
                                (ub4) strlen(sql),
                                OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (oci_error_p(lexpos(), dbh, "OCIStmtPrepare", sql, oci_status))
        return 0;

    oci_status = OCIStmtExecute(connection->svc,
                                stmt,
                                connection->err,
                                0, 0, NULL, NULL, OCI_DESCRIBE_ONLY);
    if (oci_error_p(lexpos(), dbh, "OCIStmtExecute", sql, oci_status))
        return 0;

    oci_status = OCIAttrGet(stmt,
                            OCI_HTYPE_STMT,
                            (oci_attribute_t *) & n_columns,
                            NULL, OCI_ATTR_PARAM_COUNT, connection->err);
    if (oci_error_p(lexpos(), dbh, "OCIAttrGet", sql, oci_status))
        return 0;

    ns_ora_log(lexpos(), "Starting columns");

    for (i = 0; i < n_columns; i++) {
        OCIParam *param;
        Ns_Set *cinfo;
        char name[512];
        char *name1;
        ub4 name1_size;
        /* for formatting the int returns big enough for 64 bits */
#define SBUF_BUFFER_SIZE 24
        char sbuf[SBUF_BUFFER_SIZE];
        ub2 size;
        ub2 precision;
        sb1 scale;
        OCITypeCode type;

        oci_status = OCIParamGet(stmt,
                                 OCI_HTYPE_STMT,
                                 connection->err,
                                 (oci_param_t *) & param, (ub4)i + 1);
        if (oci_error_p(lexpos(), dbh, "OCIParamGet", sql, oci_status))
            return 0;

        oci_status = OCIAttrGet(param,
                                OCI_DTYPE_PARAM,
                                (oci_attribute_t *) & name1,
                                &name1_size, OCI_ATTR_NAME,
                                connection->err);
        if (oci_error_p(lexpos(), dbh, "OCIAttrGet", sql, oci_status))
            return 0;

        ns_ora_log(lexpos(), "column name %s", name1);
        memcpy(name, name1, name1_size);
        name[name1_size] = 0;
        downcase(name);

        oci_status = OCIAttrGet(param,
                                OCI_DTYPE_PARAM,
                                (oci_attribute_t *) & type,
                                NULL, OCI_ATTR_DATA_TYPE, connection->err);
        if (oci_error_p(lexpos(), dbh, "OCIAttrGet", sql, oci_status))
            return 0;

        cinfo = Ns_SetCreate(name);
        switch (type) {
        case SQLT_DAT:
            Ns_SetPut(cinfo, "type", "date");
            break;

        case SQLT_NUM:
            ns_ora_log(lexpos(), "numeric type");
            Ns_SetPut(cinfo, "type", "numeric");

            /* for numeric type we get precision and scale */
            /* The docs lie; they say the types for precision
               and scale are ub1 and sb1, but they seem to
               actually be ub2 and sb1, at least for Oracle 8.1.5. */
            oci_status = OCIAttrGet(param,
                                    OCI_DTYPE_PARAM,
                                    (dvoid *) & precision,
                                    NULL,
                                    OCI_ATTR_PRECISION, connection->err);
            if (oci_error_p(lexpos(), dbh, "OCIAttrGet", sql, oci_status))
                return 0;

            ns_ora_log(lexpos(), "precision %d", precision);
            snprintf(sbuf, SBUF_BUFFER_SIZE, "%d", (int) precision);
            Ns_SetPut(cinfo, "precision", sbuf);

            oci_status = OCIAttrGet(param,
                                    OCI_DTYPE_PARAM,
                                    (ub1 *) & scale,
                                    NULL, OCI_ATTR_SCALE, connection->err);
            if (oci_error_p(lexpos(), dbh, "OCIAttrGet", sql, oci_status))
                return 0;

            ns_ora_log(lexpos(), "scale %d", scale);
            snprintf(sbuf, SBUF_BUFFER_SIZE, "%d", (int) scale);
            Ns_SetPut(cinfo, "scale", sbuf);

            break;

        case SQLT_INT:
            Ns_SetPut(cinfo, "type", "integer");
            break;

        case SQLT_FLT:
            /* this is potentially bogus; right thing to do is add another OCI call
               to find length and then see if it is real or double */
            Ns_SetPut(cinfo, "type", "double");
            break;

        case SQLT_CLOB:
            Ns_SetPut(cinfo, "type", "text");
            Ns_SetPut(cinfo, "lobtype", "clob");
            break;

        case SQLT_BLOB:
            Ns_SetPut(cinfo, "type", "text");
            Ns_SetPut(cinfo, "lobtype", "blob");
            break;

        default:
            Ns_SetPut(cinfo, "type", "text");
            break;
        }

        ns_ora_log(lexpos(), "asking for size");

        /* Now lets ask for the size */
        oci_status = OCIAttrGet(param,
                                OCI_DTYPE_PARAM,
                                (oci_attribute_t *) & size,
                                NULL, OCI_ATTR_DATA_SIZE, connection->err);
        if (oci_error_p(lexpos(), dbh, "OCIAttrGet", sql, oci_status))
            return 0;

        snprintf(sbuf, SBUF_BUFFER_SIZE, "%d", size);
        Ns_SetPut(cinfo, "size", sbuf);

        Ns_DbAddColumnInfo(tinfo, cinfo);
    }

    oci_status = OCIHandleFree(stmt, OCI_HTYPE_STMT);
    if (oci_error_p(lexpos(), dbh, "OCIHandleFree", sql, oci_status))
        return 0;

    return tinfo;
}
/*}}}*/

/*{{{ ora_table_list*/
/* poke around in Oracle and see what are all the possible tables */
static char *
ora_table_list(Tcl_DString * pds, Ns_DbHandle * dbh, int system_tables_p)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static char *
ora_table_list(Tcl_DString * pds, Ns_DbHandle * dbh, int system_tables_p)
{
    oci_status_t oci_status;
    ora_connection_t *connection;
    const char *sql = NULL;
    OCIStmt *stmt = NULL;

    OCIDefine *table_name_def;
    char table_name_buf[256];
    ub2 table_name_fetch_length;

    OCIDefine *owner_def;
    char owner_buf[256];
    ub2 owner_fetch_length;

    char *result = NULL;

    ns_ora_log(lexpos(), "entry (pds %p, dbh %p, system_tables_p %d)",
        pds, dbh, system_tables_p);

    assert(pds);
    assert(dbh);

    ns_ora_log(lexpos(), "user: %s", nilp(dbh->user));

    connection = dbh->connection;
    if (!connection) {
        error(lexpos(), "no connection.");
        goto bailout;
    }

    sql = (system_tables_p
           ? "select table_name, owner from all_tables"
           : "select table_name from user_tables");

    oci_status = OCIHandleAlloc(connection->env,
                                (oci_handle_t **) & stmt,
                                OCI_HTYPE_STMT, 0, NULL);
    if (oci_error_p(lexpos(), dbh, "OCIHandleAlloc", sql, oci_status)) {
        goto bailout;
    }

    oci_status = OCIStmtPrepare(stmt,
                                connection->err,
                                (const OraText *)sql,
                                (ub4) strlen(sql),
                                OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (oci_error_p(lexpos(), dbh, "OCIStmtPrepare", sql, oci_status)) {
        goto bailout;
    }


    oci_status = OCIDefineByPos(stmt,
                                &table_name_def,
                                connection->err,
                                1,
                                table_name_buf,
                                sizeof table_name_buf,
                                SQLT_STR,
                                NULL,
                                &table_name_fetch_length, NULL,
                                OCI_DEFAULT);
    if (oci_error_p(lexpos(), dbh, "OCIDefineByPos", sql, oci_status)) {
        goto bailout;
    }


    if (system_tables_p) {
        oci_status = OCIDefineByPos(stmt,
                                    &owner_def,
                                    connection->err,
                                    2,
                                    owner_buf,
                                    sizeof owner_buf,
                                    SQLT_STR,
                                    NULL,
                                    &owner_fetch_length, NULL,
                                    OCI_DEFAULT);
        if (oci_error_p(lexpos(), dbh, "OCIDefineByPos", sql, oci_status)) {
            goto bailout;
        }
    }

    oci_status = OCIStmtExecute(connection->svc,
                                stmt,
                                connection->err,
                                0, 0, NULL, NULL, OCI_COMMIT_ON_SUCCESS);

    for (;;) {
        oci_status = OCIStmtFetch(stmt,
                                  connection->err,
                                  1, OCI_FETCH_NEXT, OCI_DEFAULT);
        if (oci_status == OCI_NO_DATA)
            break;
        else if (oci_error_p(lexpos(), dbh, "OCIStmtFetch", 0, oci_status)) {
            goto bailout;
        }


        if (system_tables_p) {
            owner_buf[owner_fetch_length] = 0;
            downcase(owner_buf);

            if (strcmp(owner_buf, dbh->user))
                Tcl_DStringAppend(pds, owner_buf, owner_fetch_length);
        }

        table_name_buf[table_name_fetch_length] = 0;
        downcase(table_name_buf);

        Tcl_DStringAppend(pds, table_name_buf,
                          table_name_fetch_length + 1);

        if (system_tables_p)
            ns_ora_log(lexpos(), "table: `%s.%s'", owner_buf, table_name_buf);
        else
            ns_ora_log(lexpos(), "table: `%s'", table_name_buf);
    }


    result = pds->string;

  bailout:

    if (stmt != NULL) {
        oci_status = OCIHandleFree(stmt, OCI_HTYPE_STMT);
        oci_error_p(lexpos(), dbh, "OCIHandleFree", sql, oci_status);
    }

    return result;
}
/*}}}*/

/*{{{ ora_best_row_id*/

/*  ROWID is the always unique key for a row even when there is no
 *  primary key
 */

#if !defined(NS_AOLSERVER_3_PLUS)
static char
*ora_best_row_id(Tcl_DString * pds, Ns_DbHandle * dbh, char *table)
{
    ns_ora_log(lexpos(), "entry (pds %p, dbh %p, table %s", pds, dbh,
        nilp(table));

    Tcl_DStringAppend(pds, "rowid", 6);

    return pds->string;
}
#endif

/*}}}*/

/*{{{ ora_get_column_index*/
/*--------------------------------------------------------------------*/


/* the AOLserver3 team removed some commands that are pretty vital
 * to the normal operation of the ACS (ArsDigita Community System).
 * We include definitions for them here
 */
static int
ora_get_column_index(Tcl_Interp * interp, Ns_DbTableInfo * tinfo,
                     const char *indexStr, int *index)
{
    int result = TCL_ERROR;

    if (Tcl_GetInt(interp, indexStr, index)) {
        goto bailout;
    }

    if (*index >= tinfo->ncolumns) {
        char buffer[80];
        snprintf(buffer, sizeof(buffer), "%d", tinfo->ncolumns);

        Tcl_AppendResult(interp, buffer, " is an invalid column "
                         "index.  ", tinfo->table->name, " only has ",
                         buffer, " columns", (char*)0L);
        goto bailout;
    }

    result = TCL_OK;

  bailout:
    return (result);
}
/*}}}*/

/*{{{ ora_column_command */
/* re-implement the ns_column command */
int
ora_column_command(ClientData UNUSED(cd), Tcl_Interp *interp,
        int argc, const char *argv[])
{
    int result = TCL_ERROR;
    Ns_DbHandle *handle;
    Ns_DbTableInfo *tinfo = NULL;
    int colindex = -1;

    if (argc < 4) {
        Tcl_AppendResult(interp, "wrong # args:  should be \"",
                         argv[0], " command dbId table ?args?\"", (char*)0L);
        goto bailout;
    }

    if (Ns_TclDbGetHandle(interp, (char *)argv[2], &handle) != TCL_OK) {
        goto bailout;
    }

    /*!!! we should cache this */
    tinfo = ora_get_table_info(handle, argv[3]);
    if (tinfo == NULL) {
        Tcl_AppendResult(interp, "could not get table info for "
                         "table ", argv[3], (char*)0L);
        goto bailout;
    }

    if (!strcmp(argv[1], "count")) {
        if (argc != 4) {
            Tcl_AppendResult(interp, "wrong # of args: should be \"",
                             argv[0], " ", argv[1], " dbId table\"", (char*)0L);
            goto bailout;
        }
        Tcl_SetObjResult(interp, Tcl_NewIntObj(tinfo->ncolumns));

    } else if (!strcmp(argv[1], "exists")) {
        if (argc != 5) {
            Tcl_AppendResult(interp, "wrong # of args: should be \"",
                             argv[0], " ", argv[1],
                             " dbId table column\"", (char*)0L);
            goto bailout;
        }
        colindex = Ns_DbColumnIndex(tinfo, argv[4]);
        if (colindex < 0) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
        } else {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
        }
    } else if (!strcmp(argv[1], "name")) {
        if (argc != 5) {
            Tcl_AppendResult(interp, "wrong # of args: should be \"",
                             argv[0], " ", argv[1],
                             " dbId table column\"", (char*)0L);
            goto bailout;
        }
        if (ora_get_column_index(interp, tinfo, argv[4], &colindex)
            != TCL_OK) {
            goto bailout;
        }
        Tcl_SetObjResult(interp, Tcl_NewStringObj(tinfo->columns[colindex]->name, -1));

    } else if (!strcmp(argv[1], "type")) {
        if (argc != 5) {
            Tcl_AppendResult(interp, "wrong # of args: should be \"",
                             argv[0], " ", argv[1],
                             " dbId table column\"", (char*)0L);
            goto bailout;
        }
        colindex = Ns_DbColumnIndex(tinfo, argv[4]);
        if (colindex < 0) {
            Tcl_ResetResult(interp);
        } else {
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj(Ns_SetGet(tinfo->columns[colindex], "type"), -1));
        }
    } else if (!strcmp(argv[1], "typebyindex")) {
        if (argc != 5) {
            Tcl_AppendResult(interp, "wrong # of args: should be \"",
                             argv[0], " ", argv[1],
                             " dbId table column\"", (char*)0L);
            goto bailout;
        }
        if (ora_get_column_index(interp, tinfo, argv[4], &colindex)
            != TCL_OK) {
            goto bailout;
        }
        if (colindex < 0) {
            Tcl_ResetResult(interp);
        } else {
            Tcl_SetObjResult(interp,
                             Tcl_NewStringObj(Ns_SetGet(tinfo->columns[colindex], "type"), -1));
        }

    } else if (!strcmp(argv[1], "value")) {
        /* not used in ACS AFAIK */
        Tcl_AppendResult(interp, argv[1], " value is not implemented.",
                         (char*)0L);
        goto bailout;

    } else if (!strcmp(argv[1], "valuebyindex")) {
        /* not used in ACS AFAIK */
        Tcl_AppendResult(interp, argv[1],
                         " valuebyindex is not implemented.", (char*)0L);
        goto bailout;
    } else {
        Tcl_AppendResult(interp, "unknown command \"", argv[1],
                         "\": should be count, exists, name, "
                         "type, typebyindex, value, or "
                         "valuebyindex", (char*)0L);
        goto bailout;
    }

    result = TCL_OK;

  bailout:

    Ns_DbFreeTableInfo(tinfo);
    return (result);
}
/*}}}*/

/*{{{ ora_table_command */
/* re-implement the ns_table command */
int
ora_table_command(ClientData UNUSED(cd), Tcl_Interp *interp,
        int argc, const char *argv[])
{
    int result = TCL_ERROR;
    Tcl_DString tables_string;
    char *tables, *scan;

    Ns_DbHandle *handle;

    if (argc < 3) {
        Tcl_AppendResult(interp, "wrong # args:  should be \"",
                         argv[0], " command dbId ?args?\"", (char*)0L);
        goto bailout;
    }

    if (Ns_TclDbGetHandle(interp, (char *)argv[2], &handle) != TCL_OK) {
        goto bailout;
    }

    if (!strcmp(argv[1], "bestrowid")) {
        /* not used in ACS AFAIK */
        Tcl_AppendResult(interp, argv[1], " bestrowid is not implemented.",
                         (char*)0L);
        goto bailout;
    } else if (!strcmp(argv[1], "exists")) {
        int exists_p = 0;

        if (argc != 4) {
            Tcl_AppendResult(interp, "wrong # of args: should be \"",
                             argv[0], " ", argv[1], "dbId table\"", (char*)0L);
            goto bailout;
        }

        Tcl_DStringInit(&tables_string);

        scan = ora_table_list(&tables_string, handle, 1);

        if (scan == NULL) {
            Tcl_DStringFree(&tables_string);
            goto bailout;
        }

        while (*scan != '\000') {
            if (!strcmp(argv[3], scan)) {
                exists_p = 1;
                break;
            }
            scan += strlen(scan) + 1;
        }

        Tcl_DStringFree(&tables_string);

        if (exists_p) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
        } else {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
        }

    } else if (!strncmp(argv[1], "list", 4)) {
        int system_tables_p = 0;

        if (argc != 3) {
            Tcl_AppendResult(interp, "wrong # of args: should be \"",
                             argv[0], " ", argv[1], "dbId\"", (char*)0L);
            goto bailout;
        }

        if (!strcmp(argv[1], "listall")) {
            system_tables_p = 1;
        }

        Tcl_DStringInit(&tables_string);

        tables = ora_table_list(&tables_string, handle, system_tables_p);

        if (tables == NULL) {
            Tcl_DStringFree(&tables_string);
            goto bailout;
        }

        for (scan = tables; *scan != '\000'; scan += strlen(scan) + 1) {
            Tcl_AppendElement(interp, scan);
        }
        Tcl_DStringFree(&tables_string);

    } else if (!strcmp(argv[1], "value")) {
        /* not used in ACS AFAIK */
        Tcl_AppendResult(interp, argv[1], " value is not implemented.",
                         (char*)0L);
        goto bailout;

    } else {
        Tcl_AppendResult(interp, "unknown command \"", argv[1],
                         "\": should be bestrowid, exists, list, "
                         "listall, or value", (char*)0L);
        goto bailout;
    }

    result = TCL_OK;

  bailout:
    return (result);
}
/*}}}*/

/*{{{ Ns_DbTableInfo */
static Ns_DbTableInfo *
Ns_DbNewTableInfo(const char *table)
{
    Ns_DbTableInfo *tinfo;

    tinfo = Ns_Malloc(sizeof(Ns_DbTableInfo));

    tinfo->table = Ns_SetCreate(table);
    tinfo->ncolumns = 0;
    tinfo->size = 5;
    tinfo->columns = Ns_Malloc(sizeof(Ns_Set *) * (size_t)tinfo->size);

    return (tinfo);

}
/*}}}*/

/*{{{ Ns_DbAddColumnInfo */
static void
Ns_DbAddColumnInfo(Ns_DbTableInfo * tinfo, Ns_Set * column_info)
{
    tinfo->ncolumns++;

    if (tinfo->ncolumns > tinfo->size) {
        tinfo->size *= 2;
        tinfo->columns = Ns_Realloc(tinfo->columns,
                                    (size_t)tinfo->size * sizeof(Ns_Set *));
    }
    tinfo->columns[tinfo->ncolumns - 1] = column_info;

}
/*}}}*/

/*{{{ Ns_DbFreeTableInfo */
static void
Ns_DbFreeTableInfo(Ns_DbTableInfo * tinfo)
{
    if (tinfo != NULL) {
        int i;

        for (i = 0; i < tinfo->ncolumns; i++) {
            Ns_SetFree(tinfo->columns[i]);
        }

        Ns_SetFree(tinfo->table);
        Ns_Free(tinfo->columns);
        Ns_Free(tinfo);
    }
}
/*}}}*/

/*{{{ Ns_DbColumnIndex */
static int
Ns_DbColumnIndex(Ns_DbTableInfo * tinfo, const char *name)
{
    int i;
    int result = -1;

    for (i = 0; i < tinfo->ncolumns; i++) {
        const char *cname = tinfo->columns[i]->name;
        if ((cname == name)
            || ((cname == NULL) && (name == NULL))
            || (strcmp(cname, name) == 0)) {
            result = i;
            break;
        }
    }

    return (result);
}
/*}}}*/

#endif


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
