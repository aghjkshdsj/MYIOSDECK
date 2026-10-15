// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Runs x86 work through FEX when JIT is on; otherwise asks first, and offers
/// to enable JIT or to continue in the (much slower) interpreter.
struct JITGate: ViewModifier {
    @Binding var pending: (() -> Void)?
    @EnvironmentObject private var jit: JITController
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var settings: Settings

    func body(content: Content) -> some View {
        content.glassDialog("You are running this without JIT", isPresented: presented,
                            message: engine.interpreterLinked
                                ? "Without JIT, MYIOSDECK interprets every x86 instruction instead of translating it to ARM64 once. It works, but expect it to be many times slower. The results are saved to the log so you can share them."
                                : "This build has no interpreter, so x86 code needs JIT. Enable JIT with StikDebug to continue.") {
            var b = [DialogButton(title: "Enable JIT") {
                pending = nil
                jit.enableWithStikDebug(poolMB: settings.jitPoolMB) { engine.start(settings: settings) }
            }]
            if engine.interpreterLinked {
                let action = pending
                b.append(DialogButton(title: "Continue anyway") {
                    pending = nil
                    dlog("[app] user chose to run without JIT (interpreter)")
                    action?()
                })
            }
            b.append(.cancel { pending = nil })
            return b
        }
    }

    private var presented: Binding<Bool> {
        Binding(get: { pending != nil }, set: { if !$0 { pending = nil } })
    }
}

extension View {
    /// Set `pending` to the no-JIT action to show the prompt.
    func jitGate(pending: Binding<(() -> Void)?>) -> some View { modifier(JITGate(pending: pending)) }
}
