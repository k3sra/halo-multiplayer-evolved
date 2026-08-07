// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Engine/CampaignEngineControl.h
//
// An IEngineControl backed by the engine's own reflected campaign path.
//
// WHY THIS EXISTS
//
// The mod could always start a match. Pressing START MATCH called the engine's campaign
// entry point through UE reflection and the map loaded, which is how every single player
// launch in this project has worked.
//
// What it could not do was start one *for everybody*. That sequence lives in LobbyManager,
// which drives it through IEngineControl, and the only implementation the mod ever
// installed was the inert one that refuses every call. So the countdown never ran, no
// LaunchNow was ever broadcast, and a client sat in the lobby while the host loaded alone.
//
// This closes that gap by exposing the working path through the interface the launch
// sequence already speaks. The reflection itself stays in the Unreal layer: this takes a
// callable and knows nothing about object arrays or the game thread, which is what keeps
// Engine/ free of Unreal and keeps this testable.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "Engine/IEngineControl.h"

namespace mpe::engine {

/// Begins a scenario. Supplied by the mod, which owns the reflection and the game thread.
///
/// Returning a Result rather than a bool so a refusal carries its reason all the way to the
/// player, which is the difference between "the match did not start" and a sentence that
/// says why.
using BeginScenarioFn = std::function<Result(std::string_view scenario, bool friendly_fire)>;

class CampaignEngineControl final : public IEngineControl {
public:
    explicit CampaignEngineControl(BeginScenarioFn begin) : begin_(std::move(begin)) {}

    [[nodiscard]] EngineCapabilities Capabilities() const override {
        EngineCapabilities capabilities;
        // The one that decides whether a match can happen, and the only one this
        // implementation claims. The rest describe the Blam console, which this path does
        // not use and must not pretend to have.
        capabilities.can_begin_scenario = (begin_ != nullptr);
        return capabilities;
    }

    [[nodiscard]] EngineLifecycle Lifecycle() const override { return lifecycle_; }

    // Session configuration is the Blam console's, not this path's. Accepted rather than
    // refused, because the lobby calls these on the way to a launch and a refusal here
    // would stop a match over settings that are cosmetic next to whether the players end
    // up in the same map. Nothing is silently claimed: Capabilities reports them false.
    [[nodiscard]] Result SetSessionClass(SessionClass) override { return Result::Success(); }
    [[nodiscard]] Result SetSessionPrivacy(SessionPrivacy) override { return Result::Success(); }
    [[nodiscard]] Result SetSimulationBandwidth(std::uint32_t) override {
        return Result::Success();
    }
    [[nodiscard]] Result SetHostMigrationEnabled(bool) override { return Result::Success(); }

    [[nodiscard]] Result ApplyMatchSettings(const MatchSettings& settings) override {
        // Kept, not applied. The scenario and the friendly fire flag are handed to the
        // campaign entry point when the load begins, which is the only moment the engine
        // will take them.
        settings_ = settings;
        return Result::Success();
    }

    [[nodiscard]] Result BeginLoadScenario(std::string_view scenario,
                                           std::uint32_t /*seed*/) override {
        if (begin_ == nullptr) {
            return Result::Fail(ErrorCode::InvalidState,
                                "no campaign entry point was supplied to the engine control");
        }
        lifecycle_ = EngineLifecycle::Loading;

        // The scenario the lobby agreed on wins over anything held locally, because on a
        // client the two are only the same if the host's settings have already arrived.
        const std::string wanted(scenario.empty() ? settings_.scenario : std::string(scenario));
        const Result      began = begin_(wanted, settings_.friendly_fire);
        if (!began.ok()) {
            lifecycle_ = EngineLifecycle::Faulted;
            return began;
        }
        return Result::Success();
    }

    [[nodiscard]] Expected<float> QueryLoadProgress() const override {
        // The campaign entry point does not report progress, and inventing a curve would
        // be a number that means nothing. It returns having committed to the load, so from
        // the lobby's point of view this machine is as loaded as it can report being; the
        // launch waits on every peer reaching that same point.
        return lifecycle_ == EngineLifecycle::Loading || lifecycle_ == EngineLifecycle::InMatch
                   ? 1.0f
                   : 0.0f;
    }

    [[nodiscard]] Result LaunchMatch() override {
        // Beginning the scenario is the launch. There is no separate release step to make,
        // so this records that every peer got there rather than doing anything further.
        lifecycle_ = EngineLifecycle::InMatch;
        return Result::Success();
    }

    [[nodiscard]] Result EndMatch() override {
        lifecycle_ = EngineLifecycle::PostMatch;
        return Result::Success();
    }
    [[nodiscard]] Result ReturnToFrontEnd() override {
        lifecycle_ = EngineLifecycle::Idle;
        return Result::Success();
    }

    // Everything below belongs to the map variant work, which this path does not reach.
    // Refused rather than accepted, because a caller that gets a success here would go on
    // to place objects that never appear.
    [[nodiscard]] Result LoadMapVariant(std::string_view) override { return NotSupported(); }
    [[nodiscard]] Result ClearSandbox() override { return NotSupported(); }
    [[nodiscard]] Expected<SandboxObjectHandle> SpawnSandboxObject(
        const SandboxPlacement&) override {
        return Error{ErrorCode::NotImplemented, kNoSandbox};
    }
    [[nodiscard]] Result DespawnSandboxObject(SandboxObjectHandle) override {
        return NotSupported();
    }
    [[nodiscard]] Expected<std::int32_t> ResolvePaletteIndex(std::string_view) const override {
        return Error{ErrorCode::NotImplemented, kNoSandbox};
    }
    [[nodiscard]] Result ExecuteConsoleCommand(std::string_view) override {
        return Result::Fail(ErrorCode::NotImplemented,
                            "this build's Blam console binding is not resolved; the match "
                            "path does not use it");
    }

private:
    static constexpr const char* kNoSandbox =
        "sandbox editing needs the map variant path, which this build does not reach";

    [[nodiscard]] static Result NotSupported() {
        return Result::Fail(ErrorCode::NotImplemented, kNoSandbox);
    }

    BeginScenarioFn begin_;
    MatchSettings   settings_;
    EngineLifecycle lifecycle_{EngineLifecycle::Idle};
};

} // namespace mpe::engine
