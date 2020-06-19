import Darwin
import comdb2

var db = Comdb2(dbname: "mikedb", tier: "local")
if db == nil {
    print("Can't connect to db")
    exit(0)
}
var result = db!.run(sql: "select a, 'somestring' as b, null as c from t order by a limit 10")
if case let Comdb2QueryResults.error(errcode: rc, errmsg: errmsg) = result {
    print("got rc \(rc): \(errmsg)")
    exit(0)
}
else if case let Comdb2QueryResults.rows(rows) = result {
    for row in rows {
        if case let Comdb2Row.values(rowValues) = row {
            var colNum = 0
            let colNames = rows.columnNames()
            var sep = ""
            for col in 0 ..< rowValues.count {
                print("\(sep)\(colNames[colNum])=", terminator:"")
                if rowValues[col] == nil {
                    print("null", terminator: "")
                }
                else {
                    print("\(rowValues[col]!)", terminator:"")
                }
                sep = ", "
                colNum += 1
            }
            print("")
        }
        else if case let Comdb2Row.error(errcode: rc, errmsg: errmsg) = row {
            print("got rc \(rc): \(errmsg)")
        }
    }
}
