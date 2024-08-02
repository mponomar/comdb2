#!/opt/bb/bin/env python

import sqlquery_pb2
import sqlresponse_pb2
import sys
import json
import struct

inmsgtypes = {
        108: 'reset',
        1: 'query',
        121: 'ssl'
}

outmsgtypes = {
    205: 'heartbeat',
    1002: 'sql response',
    1005: 'dbinfo',
    1006: 'effects',
    1007: 'ping',
    1008: 'pong',
    1009: 'trace',
    1010: 'ssl'
}

headers={}

while True:
    line=sys.stdin.readline()
    if not line:
        break
    obj = json.loads(line)
    if 'type' in obj and obj['type'] == 'net':
        if not 'dir' in obj or not 'msg' in obj or not 'data' in obj or not 'id' in obj:
            continue
        data = bytearray.fromhex(obj['data'][2:-1])
        if obj['dir'] == 'in':
            sys.stdout.write("[%d]->" % (obj['id']))
        else:
            sys.stdout.write("[%d]<-" % (obj['id']))
        if obj['msg'] == 'hrtbt':
            sys.stdout.write("heartbeat\n")
        elif obj['msg'] == 'header':
            msgtype, unused, state, length = struct.unpack('iiii', data)
            if not msgtype in inmsgtypes:
                sys.stdout.write('unknown request type %d\n' % (msgtype))
            else:
                sys.stdout.write('request: %s\n' % (inmsgtypes[msgtype]))
        elif obj['msg'] == 'payload':
            msg = sqlquery_pb2.CDB2_QUERY()
            msg.ParseFromString(data)
            print(msg)
            try:
                if msg.HasField('sql_query'):
                    # forget types if a new request
                    headers[obj['id']] = None
            except ValueError:
                # may not have an sql_query field
                pass
        elif obj['msg'] == 'rsphdr':
            try:
                msgtype, unused, state, length = struct.unpack('>iiii', data)
                if not msgtype in outmsgtypes:
                    sys.stdout.write('unknown response type %d\n' % (msgtype))
                else:
                    sys.stdout.write('response: %s\n' % (outmsgtypes[msgtype]))
            except:
                    sys.stdout.write('unknown response: %s\n' % (obj['data'][2:-1]))
        elif obj['msg'] == 'rsp':
            msg = sqlresponse_pb2.CDB2_SQLRESPONSE()
            msg.ParseFromString(data)
            if msg.response_type == sqlresponse_pb2.COLUMN_NAMES:
                headers[obj['id']] = msg
            print(msg)
            if msg.response_type == sqlresponse_pb2.COLUMN_VALUES:
                print("headers for msg:", headers[obj['id']])
