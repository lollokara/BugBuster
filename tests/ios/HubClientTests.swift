import Foundation

private var failures = 0
private func expect<T: Equatable>(_ got: T, _ want: T, _ what: String, line: UInt = #line) {
    if got != want { failures += 1; print("FAIL (line \(line)): \(what) - got \(got), want \(want)") }
}

final class StubProtocol: URLProtocol {
    nonisolated(unsafe) static var handler: ((URLRequest) throws -> (Int, Data))?
    nonisolated(unsafe) static var requests: [URL] = []
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        StubProtocol.requests.append(request.url!)
        do {
            let (code, data) = try StubProtocol.handler!(request)
            let resp = HTTPURLResponse(url: request.url!, statusCode: code, httpVersion: nil, headerFields: nil)!
            client?.urlProtocol(self, didReceive: resp, cacheStoragePolicy: .notAllowed)
            client?.urlProtocol(self, didLoad: data)
            client?.urlProtocolDidFinishLoading(self)
        } catch {
            client?.urlProtocol(self, didFailWithError: error)
        }
    }
    override func stopLoading() {}
}

private let seriesJSON = #"{"bucket":60,"t":[600],"v_min":[3.5],"v_avg":[3.7],"v_max":[3.9],"i_min":[0.1],"i_avg":[0.2],"i_max":[0.3],"soc":[80],"res":[60]}"#

func runHubClientTests() async {
    let cfg = URLSessionConfiguration.ephemeral
    cfg.protocolClasses = [StubProtocol.self]
    let session = URLSession(configuration: cfg)
    var clock = Date(timeIntervalSince1970: 1_000_000)
    let client = HubClient(baseURL: URL(string: "http://192.168.3.87:8080")!, session: session, now: { clock })

    // url settings
    expect(HubSettings.normalize(" http://192.168.3.87:8080/ ")?.absoluteString, "http://192.168.3.87:8080", "trailing slash dropped")
    expect(HubSettings.normalize("https://hub.lan") == nil, true, "https refused")
    expect(HubSettings.normalize("http://hub.lan/path") == nil, true, "path refused")
    expect(HubSettings.normalize("192.168.3.87") == nil, true, "scheme required")
    expect(HubSettings.normalize("") == nil, true, "empty refused")

    // success + query building
    StubProtocol.handler = { _ in (200, Data(seriesJSON.utf8)) }
    let s = try? await client.series(uid: "a0b765112233-7-1791000000", from: 100, to: 200.5, bucket: 5)
    expect(s?.points.count, 1, "series decoded")
    let url = StubProtocol.requests.last!.absoluteString
    expect(url, "http://192.168.3.87:8080/api/v1/runs/a0b765112233-7-1791000000/samples?from=100.000&to=200.500&bucket=5", "request url")

    // 404 for an unknown run is "the hub has nothing", not a failure that disables the hub
    StubProtocol.handler = { _ in (404, Data()) }
    let ext = try? await client.extent(uid: "a0b765112233-9-1")
    expect(ext == nil, true, "unknown run -> nil extent")
    expect(client.isKnownUnreachable, false, "404 does not mark the hub unreachable")

    // coverage decode
    StubProtocol.handler = { _ in (200, Data(#"{"ranges":[{"from":1,"to":2,"res":1,"points":1}]}"#.utf8)) }
    expect((try? await client.coverage(uid: "u"))?.ranges.count, 1, "coverage decoded")

    // unreachable: first call hits the network once, the next ones fail instantly for 30 s
    StubProtocol.requests = []
    StubProtocol.handler = { _ in throw URLError(.timedOut) }
    do { _ = try await client.series(uid: "u"); expect(false, true, "must throw") } catch { expect(error as? HubError, .unreachable, "timeout -> unreachable") }
    do { _ = try await client.series(uid: "u") } catch { expect(error as? HubError, .unreachable, "negative cache") }
    expect(StubProtocol.requests.count, 1, "second call never touched the network")
    expect(client.isKnownUnreachable, true, "flagged")
    clock = clock.addingTimeInterval(31)
    StubProtocol.handler = { _ in (200, Data(seriesJSON.utf8)) }
    expect((try? await client.series(uid: "u"))?.points.count, 1, "retries after the negative cache expires")
    expect(client.isKnownUnreachable, false, "recovered")

    // 503 (hub up, database down) is treated like unreachable
    StubProtocol.handler = { _ in (503, Data()) }
    do { _ = try await client.series(uid: "u"); expect(false, true, "must throw") } catch { expect(error as? HubError, .http(503), "503 surfaces") }
    expect(client.isKnownUnreachable, true, "503 backs off")

    // no base url
    let none = HubClient(baseURL: nil, session: session, now: { clock })
    do { _ = try await none.coverage(uid: "u") } catch { expect(error as? HubError, .unreachable, "no url -> unreachable") }

    if failures > 0 { print("\(failures) failure(s)") }
}

@main
struct HubClientTestMain {
    static func main() async {
        await runHubClientTests()
        exit(failures == 0 ? 0 : 1)
    }
}
