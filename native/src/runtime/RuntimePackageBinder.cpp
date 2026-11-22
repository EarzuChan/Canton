#include <runtime/RuntimePackageBinder.h>

#include <core/InputIntent.h>
#include <runtime/CantonRuntime.h>
#include <runtime/DiagnosticsState.h>
#include <runtime/RuntimeSessionState.h>

#include <utility>

namespace canton::runtime {
    namespace {
        canton::core::InputIntent packageIntent(PackageLoadOutcome::IntentKind kind, canton::core::MutationTransaction transaction) {
            switch (kind) {
                case PackageLoadOutcome::IntentKind::Reload:
                    return canton::core::InputIntent::packageLoad(std::move(transaction));
                case PackageLoadOutcome::IntentKind::HmrReload:
                    return canton::core::InputIntent::packageLoad(std::move(transaction));
                case PackageLoadOutcome::IntentKind::PackageLoad:
                    return canton::core::InputIntent::packageLoad(std::move(transaction));
            }
            return canton::core::InputIntent::packageLoad(std::move(transaction));
        }

        canton::core::InputIntent packageFailureIntent(PackageLoadOutcome::IntentKind kind) {
            switch (kind) {
                case PackageLoadOutcome::IntentKind::Reload:
                    return canton::core::InputIntent::reload();
                case PackageLoadOutcome::IntentKind::HmrReload:
                    return canton::core::InputIntent::hmrReload();
                case PackageLoadOutcome::IntentKind::PackageLoad:
                    return canton::core::InputIntent::diagnostics();
            }
            return canton::core::InputIntent::diagnostics();
        }
    }

    void RuntimePackageBinder::apply(PackageLoadOutcome outcome, RuntimeSessionState& session, CantonRuntime& runtime, DiagnosticsState& diagnostics) const {
        if (outcome.pending) return;
        session.reset(runtime, diagnostics);

        if (outcome.error) {
            diagnostics.setError(std::move(*outcome.error));
        }

        for (const auto& log : outcome.logs) core::Log::write(log.level, TAG, log.message);
        for (auto& toast : outcome.toasts) deliverToast(diagnostics, runtime, std::move(toast));

        if (!outcome.loaded) {
            runtime.enqueueIntent(packageFailureIntent(outcome.intentKind));
            return;
        }

        runtime.setScriptHost(std::move(outcome.scriptHost));
        if (outcome.initialTransaction) {
            runtime.enqueueIntent(packageIntent(outcome.intentKind, std::move(*outcome.initialTransaction)));
        }
        session.markLoaded();
    }

    void RuntimePackageBinder::deliverToast(DiagnosticsState& diagnostics, CantonRuntime& runtime, RuntimeLoadToast toast) {
        if (DiagnosticsToast::show(diagnostics, toast.level, TAG, std::move(toast.title), std::move(toast.message), toast.coalesce)) runtime.enqueueIntent(canton::core::InputIntent::diagnostics());
    }
}

