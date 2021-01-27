#!/usr/bin/env bash

db=$1
db=$2

cdb2sql $db "drop table t"

cdb2sql $db "create table t {
    schema {
        int a
        int b
    }

    tag \"t\" {
        int a
        int b
    }
}"

echo

cdb2sql $db "dryrun alter table t {
    schema {
        int a
        int b
        int c  dbstore=1
    }

    tag \"t\" {
        int a
        int b
    }
}"

echo

cdb2sql $db "dryrun alter table t {
    schema {
        int a
        int b
    }

    tag \"t\" {
        int b
        int a
    }
}"

echo

cdb2sql $db "dryrun alter table t {
    schema {
        int a
        int b
    }
}"
