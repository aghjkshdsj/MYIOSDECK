// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 04 §I step 2: downloading Valve's public Steam installer.
//
// An ephemeral URLSession with no cookies, no credential store and no cache; 60 s request and
// 600 s resource timeouts. Redirects are followed only when `SteamHost.isAllowedRedirect`
// accepts the new URL. Authentication challenges other than the server's TLS trust are
// refused, so no credential is ever sent.

import Foundation

final class SteamHostInstallerDownload: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var refusedHost: String?   // guarded by `lock`

    /// The body of a 200 answer for `url`. Throws SteamHostError.downloadFailed, or
    /// CancellationError when the calling task was cancelled.
    static func fetch(_ url: URL) async throws -> Data {
        let delegate = SteamHostInstallerDownload()
        let configuration = URLSessionConfiguration.ephemeral
        configuration.httpCookieStorage = nil
        configuration.httpShouldSetCookies = false
        configuration.httpCookieAcceptPolicy = .never
        configuration.urlCredentialStorage = nil
        configuration.urlCache = nil
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest = 60
        configuration.timeoutIntervalForResource = 600
        let session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
        // The session keeps its delegate until it is invalidated.
        defer { session.finishTasksAndInvalidate() }

        var request = URLRequest(url: url)
        request.httpShouldHandleCookies = false
        request.timeoutInterval = 60

        let result: (Data, URLResponse)
        do {
            result = try await session.data(for: request)
        } catch {
            throw delegate.failure(for: error)
        }
        let (data, response) = result

        // A refused redirect ends the task with the 3xx answer itself; report the redirect.
        if let host = delegate.refused() {
            throw SteamHostError.downloadFailed(.redirectRefused(host))
        }
        guard let http = response as? HTTPURLResponse else {
            dlog("[steamhost] install: the answer was not HTTP")
            throw SteamHostError.downloadFailed(.httpStatus(0))
        }
        if let final = http.url, !SteamHost.isAllowedRedirect(final) {
            throw SteamHostError.downloadFailed(.redirectRefused(final.host ?? final.absoluteString))
        }
        guard http.statusCode == 200 else {
            dlog("[steamhost] install: HTTP status \(http.statusCode)")
            throw SteamHostError.downloadFailed(.httpStatus(http.statusCode))
        }
        return data
    }

    private func refused() -> String? {
        lock.lock()
        defer { lock.unlock() }
        return refusedHost
    }

    private func failure(for error: Error) -> Error {
        if Task.isCancelled { return CancellationError() }
        if let host = refused() { return SteamHostError.downloadFailed(.redirectRefused(host)) }
        guard let urlError = error as? URLError else {
            dlog("[steamhost] install: download failed: \(error.localizedDescription)")
            return SteamHostError.downloadFailed(.network(URLError.Code.unknown.rawValue))
        }
        dlog("[steamhost] install: download failed: URLError \(urlError.code.rawValue)")
        switch urlError.code {
        case .notConnectedToInternet, .networkConnectionLost, .timedOut, .cannotFindHost,
             .cannotConnectToHost, .dnsLookupFailed, .dataNotAllowed, .internationalRoamingOff,
             .callIsActive:
            return SteamHostError.downloadFailed(.noConnection)
        default:
            return SteamHostError.downloadFailed(.network(urlError.code.rawValue))
        }
    }

    // MARK: URLSessionTaskDelegate

    func urlSession(_ session: URLSession, task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        if let next = request.url, SteamHost.isAllowedRedirect(next) {
            completionHandler(request)
            return
        }
        let host = request.url?.host ?? request.url?.absoluteString ?? "an unknown address"
        lock.lock()
        refusedHost = host
        lock.unlock()
        dlog("[steamhost] install: refused a redirect to \(host)")
        completionHandler(nil)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask,
                    didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        if challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust {
            completionHandler(.performDefaultHandling, nil)
        } else {
            completionHandler(.cancelAuthenticationChallenge, nil)
        }
    }
}
