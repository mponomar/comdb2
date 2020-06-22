import Foundation
import cdb2api

public enum Comdb2Row {
    case values(columnValues: [Any?])
    case error(error: Comdb2Error)
}

public struct Comdb2Error : Error {
    // TODO: enum?
    public let errorCode: Int
    public let errorMessage: String

    public init(errorCode: Int, errorMessage: String) {
        self.errorCode = errorCode
        self.errorMessage = errorMessage
    }
}

public class Comdb2Rows: IteratorProtocol, Sequence {
    private(set) var db: Comdb2
    public private(set) var columnNames: [String]
    private var columnValues: [Any?] = []

    public func next() -> Comdb2Row? {
        columnValues.removeAll(keepingCapacity: true)
        let rc = cdb2_next_record(db.db)
        if rc == CDB2_OK_DONE.rawValue {
            return nil
        }
        else if rc != 0 {
            // It'd be preferable to throw an error instead,
            // but then we couldn't use this as an interator
            return .error(error: Comdb2Error(errorCode: Int(rc), errorMessage: String(cString: cdb2_errstr(db.db))))
        }
        else {
            for i in 0 ..< cdb2_numcolumns(db.db) {
                let opaqueValue = cdb2_column_value(db.db, i)
                let type:UInt32 = UInt32(cdb2_column_type(db.db, i))
                var subsecondMultiplier: UInt32 = 0
                if opaqueValue == nil {
                    columnValues.append(nil)
                    continue
                }
                switch type {
                    case CDB2_INTEGER.rawValue:
                        columnValues.append(opaqueValue!.load(fromByteOffset: 0, as: Int64.self))

                    case CDB2_CSTRING.rawValue:
                        let ptr: UnsafeMutablePointer<Int8> = opaqueValue!.assumingMemoryBound(to: Int8.self)
                        columnValues.append(String(cString: ptr))

                    case CDB2_REAL.rawValue:
                        var d: Double = 0
                        withUnsafeMutablePointer(to: &d) { ptr in 
                            let p: UnsafeMutableRawPointer = UnsafeMutableRawPointer(ptr)
                            p.copyMemory(from: opaqueValue!, byteCount: 8)
                        }
                        columnValues.append(d)

                    case CDB2_BLOB.rawValue:
                        let sz = cdb2_column_size(db.db, i)
                        let ptr: UnsafeBufferPointer<UInt8> = UnsafeBufferPointer(start: opaqueValue!.assumingMemoryBound(to: UInt8.self), count: Int(sz))
                        columnValues.append(Array(ptr))

                    case CDB2_DATETIME.rawValue:
                        subsecondMultiplier = 1000000
                        fallthrough
                    case CDB2_DATETIMEUS.rawValue:
                        subsecondMultiplier = 1000
                        var cdate: cdb2_client_datetime_t = cdb2_client_datetime_t()
                        let sz = MemoryLayout.size(ofValue: cdate)
                        withUnsafeMutablePointer(to: &cdate) { ptr in 
                            let p: UnsafeMutableRawPointer = UnsafeMutableRawPointer(ptr)
                            p.copyMemory(from: opaqueValue!, byteCount: sz)
                        }
                        var tzname: String?
                        withUnsafeBytes(of: cdate.tzname) { ptr in 
                            let buf = ptr.bindMemory(to: UInt8.self)
                            tzname = String(data: Data(buf), encoding: .utf8)
                        }
                        if tzname == nil {
                            return .error(error: Comdb2Error(errorCode: -5, errorMessage: "Invalid timezone encoding"))
                        }
                        let timezone = TimeZone(identifier: tzname!)
                        if timezone == nil {
                            return .error(error: Comdb2Error(errorCode: -5, errorMessage: "Invalid timezone"))
                        }

                        let date = DateComponents(calendar: Calendar.current,
                                    timeZone: timezone,
                                    year: Int(cdate.tm.tm_year + 1900),
                                    month: Int(cdate.tm.tm_mon + 1),
                                    day: Int(cdate.tm.tm_mday),
                                    hour: Int(cdate.tm.tm_hour),
                                    minute: Int(cdate.tm.tm_min),
                                    second: Int(cdate.tm.tm_sec),
                                    nanosecond: Int(cdate.msec * subsecondMultiplier)
                                    ).date
                        columnValues.append(date)

                    default:
                        // TODO: interval types
                        return .error(error: Comdb2Error(errorCode: -5, errorMessage: "Unsupported type \(type)"))

                }
            }
            return .values(columnValues: columnValues)
        }
    }

    fileprivate init(db: Comdb2, columnNames: [String]) {
        self.db = db
        self.columnNames = columnNames
    } 
}

public class Comdb2 {
    fileprivate var db: OpaquePointer
    public init(dbName: String, tier: String = "default") throws {
        var dbp: OpaquePointer?
        let rc = cdb2_open(&dbp, dbName, tier, 0)
        db = dbp!
        if rc != 0 {
            let err = Comdb2Error(errorCode: Int(rc), errorMessage: String(cString: cdb2_errstr(db)))
            cdb2_close(db)
            throw err
        }
    }

    public func run(sql: String, params: [String : Any]? = nil) throws -> Comdb2Rows {
        let rc = cdb2_run_statement(db, sql)
        if rc != 0 {
            throw Comdb2Error(errorCode: Int(rc), errorMessage: String(cString: cdb2_errstr(db)))
        }
        var colNames: [String] = []
        for columnNumber in 0 ..< cdb2_numcolumns(db) {
            colNames.append(String(cString: cdb2_column_name(db, columnNumber)))
        }
        return Comdb2Rows(db: self, columnNames: colNames)
    }
}
