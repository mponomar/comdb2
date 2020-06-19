import cdb2api

public enum Comdb2Value {
    case integer(value: Int64)
    case string(value: String)
}

public enum Comdb2Error: Int {
    case OK = 0
    case DONE = 1
    case CONNECT_ERROR = -1
    case SYNTAX_ERROR = -3
    case UNKNOWN_ERROR = -5
}

public enum Comdb2QueryResults {
    case rows(results: Comdb2Rows)
    case error(errcode: Comdb2Error, errmsg: String)
}

public enum Comdb2Row {
    case values([Comdb2Value?])
    case error(errcode: Comdb2Error, errmsg: String)
}

public struct Comdb2Rows: Sequence, IteratorProtocol {
    private var connection: Comdb2
    public func next() -> Comdb2Row? {
        connection.next()
    }

    public func columnNames() -> [String] {
        return connection.columnNames
    }

    init(connection: Comdb2) {
        self.connection = connection
    }
}

private enum ConnectionState {
    case ready
    case running
}

public class Comdb2 {
    private var dbname: String
    private var tier: String
    private var connection: OpaquePointer?
    private(set) var columnNames: [String] = []
    private var state: ConnectionState

    public func lastErrorMessage() -> String {
        String(cString: cdb2_errstr(connection!))
    }

    public init?(dbname: String, tier: String) {
        self.dbname = dbname
        self.tier = tier

        let rc = cdb2_open(&connection, dbname, tier, 0);
        if rc != CDB2_OK.rawValue {
            return nil
        }
        state = .ready
    }

    private func columnValue(forColumn n: Int32) -> Comdb2Value? {
        let val = cdb2_column_value(connection, n)
        if val == nil {
            return nil
        }
        let type: UInt32 = UInt32(cdb2_column_type(connection, n))
        switch type {
            case CDB2_INTEGER.rawValue:
                return .integer(value: val!.load(fromByteOffset: 0, as: Int64.self))
            case CDB2_CSTRING.rawValue:
                let ptr: UnsafeMutablePointer<Int8> = val!.assumingMemoryBound(to: Int8.self)
                return .string(value: String(cString: ptr))
            default:
                return nil
        }
    }

    func next() -> Comdb2Row? {
        let rc = cdb2_next_record(connection);
        if rc == CDB2_OK.rawValue {
            var values: [Comdb2Value?] = []
            let ncols = cdb2_numcolumns(connection)
            for col in 0 ..< ncols {
                values.append(columnValue(forColumn: col))
            }
            return .values(values)
        }
        else if rc == CDB2_OK_DONE.rawValue {
            return nil
        }
        else {
            return .error(errcode: toError(rc), errmsg: String(cString:cdb2_errstr(connection!)))
        }
    }

    public func run(sql: String) -> Comdb2QueryResults {
        let rc = cdb2_run_statement(connection!, sql)
        if rc != CDB2_OK.rawValue {
            return .error(errcode: toError(rc), errmsg: String(cString:cdb2_errstr(connection!)))
        }
        state = .running

        let ncols = cdb2_numcolumns(connection)
        columnNames.removeAll(keepingCapacity: true)

        for col in 0 ..< ncols {
            columnNames.append(String(cString: cdb2_column_name(connection, col)))
        }

        return .rows(results: Comdb2Rows(connection: self))
    }

    deinit {
        if let db = connection {
            cdb2_close(db)
        }
    }
}

private func toError(_ rc: Int32) -> Comdb2Error {
    switch rc {
        case 0:
            return .OK
        case 1:
            return .DONE
        case -1:
            return .CONNECT_ERROR
        case -3:
            return .SYNTAX_ERROR
        default:
            return .UNKNOWN_ERROR
    }
}
