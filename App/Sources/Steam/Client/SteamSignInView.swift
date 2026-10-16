// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The Steam sign-in sheet (spec 03 §S5) and its model.
//
// Sources: spec 03 §S3, §S5; Apple SwiftUI (NavigationStack, Form, Picker .segmented,
// TextField/SecureField modifiers, Link, onChange iOS 17), CoreImage (CIFilter.qrCodeGenerator,
// correctionLevel, samplingNearest, CIContext.createCGImage).

import CoreImage
import CoreImage.CIFilterBuiltins
import SwiftUI
import UIKit

@MainActor
final class SteamSignInModel: ObservableObject {
    static let shared = SteamSignInModel()

    enum Method: String, CaseIterable, Identifiable {
        case password, qr
        var id: String { rawValue }
    }

    enum Stage: Equatable {
        case idle
        case working
        case steamGuard(SteamGuardPrompt)
        case qrWaiting
        case qrFailed
    }

    @Published private(set) var accountName: String?
    @Published var method: Method
    @Published var account = ""
    @Published var password = ""
    @Published var code = ""
    @Published private(set) var stage: Stage = .idle
    @Published private(set) var errorText: String?
    @Published private(set) var qrURL: URL?
    @Published private(set) var qrImage: UIImage?
    @Published private(set) var submittingCode = false
    /// Counts completed sign-ins; the sheet closes itself when it changes.
    @Published private(set) var completions = 0

    private var task: Task<Void, Never>?
    private var codeTask: Task<Void, Never>?
    private var authSession: SteamAuthSession?
    private var observer: NSObjectProtocol?

    private init() {
        accountName = SteamSignIn.accountName
        method = UIDevice.current.userInterfaceIdiom == .pad ? .qr : .password
        observer = NotificationCenter.default.addObserver(forName: SteamSignIn.didChange, object: nil,
                                                          queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.refreshAccount() }
        }
    }

    var isRunning: Bool { task != nil }

    var canSignIn: Bool {
        !account.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && !password.isEmpty && !isRunning
    }

    var canSubmitCode: Bool {
        code.trimmingCharacters(in: .whitespacesAndNewlines).count >= 5 && !submittingCode
    }

    func refreshAccount() {
        accountName = SteamSignIn.accountName
    }

    /// The sheet appeared: forget the last attempt's state.
    func prepare() {
        if !isRunning {
            stage = .idle
            errorText = nil
            code = ""
        }
        refreshAccount()
    }

    // MARK: Password (§S2)

    func signInWithPassword() {
        guard canSignIn else { return }
        let name = account.trimmingCharacters(in: .whitespacesAndNewlines)
        let secret = password
        password = ""                       // cleared after submitting
        start(method: "password") { [weak self] in
            let (session, prompt) = try await SteamAuthFlow.beginPassword(account: name, password: secret)
            try Task.checkCancellation()
            self?.authSession = session
            if !prompt.isEmpty { self?.stage = .steamGuard(prompt) }
            return try await SteamAuthFlow.poll(session, limit: SteamAuthFlow.passwordPollLimit,
                                                expired: .signInExpired)
        }
    }

    func submitCode() {
        guard canSubmitCode, case .steamGuard(let prompt) = stage, let kind = prompt.code,
              let session = authSession else { return }
        let value = code
        submittingCode = true
        errorText = nil
        codeTask?.cancel()
        codeTask = Task { [weak self] in
            do {
                try await SteamAuthFlow.submitCode(value, kind: kind, session: session)
            } catch is CancellationError {
            } catch {
                // Polling keeps running, so the user can correct the code and try again.
                self?.errorText = SteamSignIn.message(error)
            }
            self?.submittingCode = false
        }
    }

    func startOver() {
        cancel()
        stage = .idle
        code = ""
        errorText = nil
    }

    // MARK: QR (§S3)

    func startQR() {
        start(method: "qr") { [weak self] in
            let session = try await SteamAuthFlow.beginQR()
            try Task.checkCancellation()
            self?.authSession = session
            self?.show(challenge: session.challengeURL)
            self?.stage = .qrWaiting
            return try await SteamAuthFlow.poll(session, limit: SteamAuthFlow.qrPollLimit,
                                                expired: .qrExpired) { url in
                self?.show(challenge: url)
            }
        }
    }

    private func show(challenge: String) {
        qrURL = URL(string: challenge)
        qrImage = SteamSignInModel.qrImage(for: challenge)
    }

    nonisolated static func qrImage(for text: String) -> UIImage? {
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(text.utf8)
        filter.correctionLevel = "M"
        guard let output = filter.outputImage, output.extent.width > 0 else { return nil }
        let scale = max(1, (720 / output.extent.width).rounded(.down))
        let scaled = output.samplingNearest().transformed(by: CGAffineTransform(scaleX: scale, y: scale))
        let context = CIContext()
        guard let cgImage = context.createCGImage(scaled, from: scaled.extent) else { return nil }
        return UIImage(cgImage: cgImage)
    }

    // MARK: Running one sign-in at a time

    private func start(method: String, _ body: @escaping @MainActor () async throws -> SteamSignInTokens) {
        cancel()
        errorText = nil
        stage = .working
        task = Task { [weak self] in
            do {
                let tokens = try await body()
                try Task.checkCancellation()
                try SteamSignIn.store(tokens)
                SteamLog.event("[steam-signin] signed in method=\(method)")
                self?.finished()
            } catch {
                if error is CancellationError || Task.isCancelled { return }
                SteamLog.event("[steam-signin] sign-in failed method=\(method) reason=\(SteamSignIn.reason(error))")
                self?.failed(error, qr: method == "qr")
            }
        }
    }

    private func finished() {
        task = nil
        codeTask?.cancel()
        codeTask = nil
        authSession = nil
        stage = .idle
        code = ""
        qrImage = nil
        qrURL = nil
        refreshAccount()
        completions += 1
    }

    private func failed(_ error: Error, qr: Bool) {
        task = nil
        codeTask?.cancel()
        codeTask = nil
        authSession = nil
        submittingCode = false
        errorText = SteamSignIn.message(error)
        stage = qr ? .qrFailed : .idle
        if qr { qrImage = nil }
    }

    /// Cancels any running sign-in (switching method, closing the sheet, starting another).
    func cancel() {
        task?.cancel()
        task = nil
        codeTask?.cancel()
        codeTask = nil
        authSession = nil
        submittingCode = false
        switch stage {
        case .working, .steamGuard, .qrWaiting: stage = .idle
        case .idle, .qrFailed: break
        }
    }

    func signOut() {
        cancel()
        SteamSignIn.signOut()
        accountName = nil
    }
}

/// The sign-in sheet, presented by the app with `.sheet { SteamSignInView() }`.
struct SteamSignInView: View {
    @ObservedObject private var model = SteamSignInModel.shared
    @Environment(\.dismiss) private var dismiss

    init() {}

    var body: some View {
        NavigationStack {
            Form {
                if let name = model.accountName {
                    signedIn(name)
                } else {
                    Section {
                        Picker("Sign in with", selection: $model.method) {
                            Text("Password").tag(SteamSignInModel.Method.password)
                            Text("QR code").tag(SteamSignInModel.Method.qr)
                        }
                        .pickerStyle(.segmented)
                    }
                    switch model.method {
                    case .password: passwordSections
                    case .qr: qrSection
                    }
                }
                if let error = model.errorText {
                    Section {
                        Text(error).foregroundStyle(.red)
                    }
                }
                Section {
                } footer: {
                    Text("Your password goes only to Steam and is never stored. The sign-in token is kept in this device's Keychain until you sign out.")
                }
            }
            .navigationTitle("Steam")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(model.accountName == nil ? "Cancel" : "Done") {
                        model.cancel()
                        dismiss()
                    }
                }
            }
        }
        .onAppear { model.prepare() }
        .onDisappear { model.cancel() }
        .onChange(of: model.method) {
            model.cancel()
            if model.method == .qr, model.accountName == nil { model.startQR() }
        }
        .onChange(of: model.completions) { dismiss() }
        .task {
            if model.method == .qr, model.accountName == nil, !model.isRunning { model.startQR() }
        }
    }

    private func signedIn(_ name: String) -> some View {
        Section {
            LabeledContent("Account", value: name)
            Button("Sign out", role: .destructive) { model.signOut() }
        } footer: {
            Text("Signing out removes the sign-in token from this device's Keychain.")
        }
    }

    @ViewBuilder
    private var passwordSections: some View {
        if case .steamGuard(let prompt) = model.stage {
            steamGuardSection(prompt)
        } else {
            Section {
                TextField("Account name", text: $model.account)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .textContentType(.username)
                SecureField("Password", text: $model.password)
                    .textContentType(.password)
                    .onSubmit { model.signInWithPassword() }
                if model.stage == .working {
                    HStack {
                        ProgressView()
                        Text("Signing in…")
                    }
                }
                Button("Sign in") { model.signInWithPassword() }
                    .disabled(!model.canSignIn)
            } footer: {
                Text("Your Steam account name can differ from the email address you use with Steam.")
            }
        }
    }

    private func steamGuardSection(_ prompt: SteamGuardPrompt) -> some View {
        Section {
            if let kind = prompt.code {
                Text(codeInstruction(kind, hint: prompt.emailHint))
                TextField("Code", text: $model.code)
                    .textContentType(.oneTimeCode)
                    .keyboardType(.asciiCapable)
                    .textInputAutocapitalization(.characters)
                    .autocorrectionDisabled()
                    .font(.body.monospaced())
                    .onSubmit { model.submitCode() }
                Button("Continue") { model.submitCode() }
                    .disabled(!model.canSubmitCode)
            }
            if prompt.approval {
                HStack(spacing: 12) {
                    ProgressView()
                    Text(prompt.code == nil ? "Approve the sign-in in the Steam app."
                                            : "Or approve the sign-in in the Steam app.")
                }
            }
            Button("Start over") { model.startOver() }
        } header: {
            Text("Steam Guard")
        }
    }

    private func codeInstruction(_ kind: SteamGuardPrompt.CodeKind, hint: String?) -> String {
        switch kind {
        case .device:
            return "Enter the code shown in the Steam mobile app."
        case .email:
            if let hint { return "Enter the code Steam sent to your email (\(hint))." }
            return "Enter the code Steam sent to your email."
        }
    }

    private var qrSection: some View {
        Section {
            if let image = model.qrImage {
                Image(uiImage: image)
                    .interpolation(.none)
                    .resizable()
                    .scaledToFit()
                    .frame(maxWidth: 240, maxHeight: 240)
                    .padding(12)
                    .background(Color.white)
                    .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                    .frame(maxWidth: .infinity)
                    .accessibilityLabel("Steam sign-in QR code")
            }
            Text("Open the Steam app on your phone, tap the Steam Guard shield, and scan this code.")
            if let url = model.qrURL {
                Link("Open in the Steam app on this device", destination: url)
            }
            switch model.stage {
            case .qrWaiting:
                HStack(spacing: 12) {
                    ProgressView()
                    Text("Waiting for approval…")
                }
            case .qrFailed:
                Button("Get a new code") { model.startQR() }
            case .working:
                ProgressView().frame(maxWidth: .infinity)
            default:
                EmptyView()
            }
        }
    }
}
