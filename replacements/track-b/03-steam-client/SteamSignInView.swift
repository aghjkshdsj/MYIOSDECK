// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import SwiftUI
import UIKit
import CoreImage

@MainActor final class SteamSignInModel: ObservableObject {
    static let shared = SteamSignInModel()
    enum Method: String, CaseIterable { case password = "Password", qr = "QR code" }
    @Published private(set) var accountName: String? = SteamSignIn.accountName
    @Published var method: Method = UIDevice.current.userInterfaceIdiom == .pad ? .qr : .password
    @Published private(set) var busy = false
    @Published private(set) var codeType: Int?
    @Published private(set) var emailHint = ""
    @Published private(set) var approval = false
    @Published private(set) var qrURL: URL?
    @Published private(set) var qrImage: UIImage?
    @Published private(set) var errorText: String?
    @Published private(set) var completion = 0
    private var flow: BSteamAuthFlow?
    private var task: Task<Void, Never>?
    private var codeTask: Task<Void, Never>?
    private var ticket: UInt64 = 0
    private var observer: NSObjectProtocol?
    private init() {
        observer = NotificationCenter.default.addObserver(forName: SteamSignIn.didChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.accountName = SteamSignIn.accountName }
        }
    }
    func cancel() {
        ticket &+= 1; task?.cancel(); codeTask?.cancel(); task = nil; codeTask = nil; flow = nil; busy = false
        codeType = nil; approval = false
    }
    func signOut() { cancel(); SteamSignIn.signOut(); accountName = nil }
    func startOver() { cancel(); errorText = nil; qrURL = nil; qrImage = nil }
    func switched() { startOver(); if method == .qr && accountName == nil { begin(account: nil, password: nil) } }
    func begin(account: String?, password: String?) {
        cancel(); let generation = ticket
        busy = true; errorText = nil; qrURL = nil; qrImage = nil
        let methodName = account == nil ? "qr" : "password", friendly = UIDevice.current.name
        task = Task {
            do {
                let flow = try await BSteamAuthFlow.begin(account: account, password: password, friendly: friendly)
                try Task.checkCancellation(); guard generation == ticket else { return }; self.flow = flow
                let confirmations = flow.confirmations
                codeType = confirmations.contains(where: { $0.type == 3 }) ? 3 : confirmations.contains(where: { $0.type == 2 }) ? 2 : nil
                emailHint = confirmations.first(where: { $0.type == 2 })?.hint ?? ""
                approval = confirmations.contains { $0.type == 4 || $0.type == 5 }
                if methodName == "qr" { setQR(flow.challenge) }
                let credentials = try await flow.poll { [weak self] url in
                    await MainActor.run { if self?.ticket == generation { self?.setQR(url) } }
                }
                try Task.checkCancellation(); guard generation == ticket else { return }
                try BSteamTokenStore.store(credentials); accountName = SteamSignIn.accountName
                SteamLog.event("[steam-signin] signed in method=\(methodName)")
                busy = false; completion &+= 1
            } catch is CancellationError {} catch {
                guard generation == ticket else { return }
                errorText = SteamSignIn.message(error); busy = false
                SteamLog.event("[steam-signin] sign-in failed method=\(methodName) reason=\(SteamSignIn.reason(error))")
            }
        }
    }
    func submit(_ code: String) {
        guard let flow = flow, let type = codeType else { return }
        let generation = ticket; codeTask?.cancel(); errorText = nil
        codeTask = Task {
            do { try await flow.submit(code, type: type) }
            catch is CancellationError {} catch {
                if generation == ticket { errorText = SteamSignIn.message(error) }
            }
        }
    }
    private func setQR(_ string: String) {
        guard string.utf8.count <= 4096, let url = URL(string: string), url.scheme?.lowercased() == "https", url.host != nil else { return }
        qrURL = url
        guard let filter = CIFilter(name: "CIQRCodeGenerator") else { return }
        filter.setValue(Data(string.utf8), forKey: "inputMessage"); filter.setValue("M", forKey: "inputCorrectionLevel")
        guard let image = filter.outputImage?.transformed(by: CGAffineTransform(scaleX: 8, y: 8)),
              let cg = CIContext().createCGImage(image, from: image.extent) else { return }
        qrImage = UIImage(cgImage: cg)
    }
}

@MainActor struct SteamSignInView: View {
    @Environment(\.dismiss) private var dismiss
    @StateObject private var model = SteamSignInModel.shared
    @State private var account = ""
    @State private var password = ""
    @State private var code = ""
    init() {}
    var body: some View {
        NavigationStack {
            Form {
                if let name = model.accountName {
                    Section { Text(name); Button("Sign out", role: .destructive) { model.signOut() } }
                    footer: { Text("Signing out removes the token from this device's Keychain.") }
                } else {
                    Section { Picker("Sign in with", selection: $model.method) { ForEach(SteamSignInModel.Method.allCases, id: \.self) { Text($0.rawValue).tag($0) } }.pickerStyle(.segmented) }
                    if model.method == .password {
                        if model.codeType != nil || model.approval {
                            Section {
                                if model.codeType != nil {
                                    TextField("Steam Guard code", text: $code).textContentType(.oneTimeCode).textInputAutocapitalization(.characters).autocorrectionDisabled().font(.system(.body, design: .monospaced))
                                    Button("Continue") { model.submit(code); code = "" }.disabled(code.trimmingCharacters(in: .whitespacesAndNewlines).count < 5)
                                    if model.codeType == 2 && !model.emailHint.isEmpty { Text(model.emailHint).font(.footnote) }
                                }
                                if model.approval { HStack { ProgressView(); Text("Approve in the Steam app") } }
                                Button("Start over") { code = ""; model.startOver() }
                            }
                        } else {
                            Section {
                                TextField("Account name", text: $account).textContentType(.username).textInputAutocapitalization(.never).autocorrectionDisabled()
                                SecureField("Password", text: $password).textContentType(.password)
                                Button("Sign in") { let secret = password; password = ""; model.begin(account: account, password: secret) }
                                    .disabled(account.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || password.isEmpty || model.busy)
                                if model.busy { ProgressView("Signing in…") }
                            } footer: { Text("Your Steam account name can differ from your email address.") }
                        }
                    } else {
                        Section {
                            if let image = model.qrImage { Image(uiImage: image).interpolation(.none).resizable().scaledToFit().padding().background(.white).clipShape(RoundedRectangle(cornerRadius: 12)).accessibilityLabel("Steam sign-in QR code") }
                            Text("Scan this code with the Steam mobile app, then approve the sign-in.")
                            if let url = model.qrURL { Link("Open in the Steam app on this device", destination: url) }
                            if model.busy { HStack { ProgressView(); Text("Waiting for approval…") } }
                            else { Button("Get a new code") { model.begin(account: nil, password: nil) } }
                        }
                    }
                    if let error = model.errorText { Section { Text(error).foregroundStyle(.red) } }
                    Section {} footer: { Text("Your password goes only to Steam and is never stored. The sign-in token stays in this device's Keychain until you sign out.") }
                }
            }
            .navigationTitle("Steam")
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button(model.accountName == nil ? "Cancel" : "Done") { model.cancel(); dismiss() } } }
            .onChange(of: model.method) { _, _ in password = ""; code = ""; model.switched() }
            .onChange(of: model.completion) { _, _ in dismiss() }
            .onAppear { if model.method == .qr && model.accountName == nil && !model.busy { model.begin(account: nil, password: nil) } }
            .onDisappear { password = ""; code = ""; model.cancel() }
        }
    }
}
