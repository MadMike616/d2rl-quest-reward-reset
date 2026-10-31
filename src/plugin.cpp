#include <D2RLPlugin/api.h>
#include <D2RLPlugin/inventory.h>
#include <D2RLPlugin/shared_events.h>
#include <D2RLPlugin/threads.h>
#include "installer.h"

#include <Windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>

namespace QuestRewardReset {

constexpr std::uint32_t AkaraItemCode = D2RL::Items::MakeItemCode("rt1");
constexpr std::uint32_t CharsiItemCode = D2RL::Items::MakeItemCode("rt2");
constexpr std::uint32_t LarzukItemCode = D2RL::Items::MakeItemCode("rt3");

struct TokenDefinition {
	std::uint32_t itemCode;
	const char* name;
	const char* tooltip;
	std::int32_t questId;
};

constexpr TokenDefinition Tokens[] {
	{ AkaraItemCode, "Akara respec", "Right Click to reset Akara Reward", 0x29 },
	{ CharsiItemCode, "Charsi imbue", "Right Click to reset Charsi Reward", 3 },
	{ LarzukItemCode, "Larzuk socket", "Right Click to reset Larzuk Reward", 0x23 },
};

constexpr std::uintptr_t GetQuestStateRva = 0x325C50;
constexpr std::uintptr_t SetQuestStateRva = 0x325C00;
constexpr std::uintptr_t ClearQuestStateRva = 0x325990;
constexpr std::uintptr_t ClearQuestCallerWitnessRva = 0x4204C7;
constexpr std::uintptr_t SetQuestCallerWitnessRva = 0x4204D7;
constexpr std::uintptr_t SynchronizeQuestFlagsRva = 0x518620;
constexpr std::uintptr_t SynchronizeQuestCallerOneRva = 0x515CB9;
constexpr std::uintptr_t SynchronizeQuestCallerTwoRva = 0x515EDC;
constexpr std::uintptr_t SynchronizeQuestCallerThreeRva = 0x515F6F;
constexpr std::uintptr_t GetLocalDataContextRva = 0x08B2D0;
constexpr std::uintptr_t GetLocalPlayerRva = 0x09A480;
constexpr std::uintptr_t GetUnitIdRva = 0x34A330;
constexpr std::uintptr_t GetUnitTypeRva = 0x34B9D0;
constexpr std::uintptr_t GetGameFromUnitRva = 0x48FF00;
constexpr std::uintptr_t GetServerUnitRva = 0x48FE80;
constexpr std::uintptr_t GetPlayerDataRva = 0x34B240;
constexpr std::uintptr_t GameDataLayoutWitnessRva = 0x44DF40;
constexpr std::uintptr_t AttachSoundRva = 0x491960;
constexpr std::uintptr_t GlobalClientIdHashBucketsRva = 0x2AAECA0;
constexpr std::uint16_t ItemConsumedSoundId = 6;
constexpr std::size_t GameDifficultyOffset = 0x104;
constexpr std::size_t GameDataContextOffset = 0x106;
constexpr std::size_t GameRequiredReadableSize = 0x140;
constexpr std::size_t ClientPlayerTypeOffset = 0x26C;
constexpr std::size_t ClientPlayerIdOffset = 0x270;
constexpr std::size_t ClientPlayerPointerOffset = 0x278;
constexpr std::size_t ClientNextOffset = 0x630;
constexpr std::size_t ClientGamePointerOffset = 0x2B0;
constexpr std::size_t ClientDataContextOffset = 0x6B0;
constexpr std::size_t PlayerDataQuestRecordArrayOffset = 0x40;
constexpr std::size_t ClientIdHashBucketCount = 256;
constexpr std::size_t MaximumRegisteredClients = 256;
constexpr std::int32_t PlayerUnitType = 0;
constexpr std::uint64_t HoverLifetimeMilliseconds = 1'500;
constexpr std::uint64_t PendingRequestLifetimeMilliseconds = 15'000;
constexpr std::uint64_t RightButtonReleaseLifetimeMilliseconds = 2'000;
constexpr std::int32_t RewardGrantedFlag = 0;
constexpr std::int32_t RewardPendingFlag = 1;

constexpr std::array<std::uint8_t, 32> QuestFunctionExpected {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
	0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41,
	0x8B, 0xF0, 0x8B, 0xDA, 0x48, 0x8B, 0xF9, 0x48,
	0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C, 0x24, 0x38,
};

// These callers establish the native helper ABI and the RewardGranted=0,
// RewardPending=1 selectors before any helper is called directly.
constexpr std::array<std::uint8_t, 16> ClearQuestCallerWitnessExpected {
	0x48, 0x8B, 0x4D, 0x80, 0x45, 0x33, 0xC0, 0x41,
	0x8D, 0x50, 0x29, 0xE8, 0xB9, 0x54, 0xF0, 0xFF,
};

constexpr std::array<std::uint8_t, 18> SetQuestCallerWitnessExpected {
	0x48, 0x8B, 0x4D, 0x80, 0xBA, 0x29, 0x00, 0x00,
	0x00, 0x44, 0x8D, 0x42, 0xD8, 0xE8, 0x17, 0x57,
	0xF0, 0xFF,
};

constexpr std::array<std::uint8_t, 32> SynchronizeQuestFlagsExpected {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
	0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xFA,
	0x48, 0x8B, 0xCA, 0xE8, 0x08, 0x2C, 0xE3, 0xFF,
	0x0F, 0xB6, 0x8B, 0x04, 0x01, 0x00, 0x00, 0x48,
};

constexpr std::array<std::uint8_t, 17> SynchronizeCallerOneExpected {
	0x06, 0xE8, 0x41, 0xFF, 0xE0, 0xFF, 0x48, 0x8B, 0xD6,
	0x48, 0x8B, 0xCB, 0xE8, 0x56, 0x29, 0x00, 0x00,
};

constexpr std::array<std::uint8_t, 17> SynchronizeCallerTwoExpected {
	0xCB, 0xE8, 0x2E, 0x32, 0x00, 0x00, 0x48, 0x8B, 0xD6,
	0x48, 0x8B, 0xCB, 0xE8, 0x33, 0x27, 0x00, 0x00,
};

constexpr std::array<std::uint8_t, 17> SynchronizeCallerThreeExpected {
	0xCB, 0xE8, 0x9B, 0x31, 0x00, 0x00, 0x48, 0x8B, 0xD6,
	0x48, 0x8B, 0xCB, 0xE8, 0xA0, 0x26, 0x00, 0x00,
};

constexpr std::array<std::uint8_t, 7> GetLocalDataContextExpected {
	0x8B, 0x05, 0x2E, 0x84, 0x99, 0x02, 0xC3,
};

constexpr std::array<std::uint8_t, 32> GetLocalPlayerExpected {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
	0xEC, 0x20, 0x83, 0xF9, 0x08, 0x0F, 0x83, 0x85,
	0x00, 0x00, 0x00, 0x8B, 0xD9, 0x48, 0x89, 0x5C,
	0x24, 0x38, 0x48, 0x83, 0xFB, 0x08, 0x72, 0x19,
};

constexpr std::array<std::uint8_t, 32> GetUnitIdExpected {
	0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75,
	0x1D, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C,
	0x24, 0x30, 0xE8, 0x39, 0xCA, 0xFF, 0xFF, 0x84,
	0xC0, 0x74, 0x01, 0xCC, 0xB8, 0xFF, 0xFF, 0xFF,
};

constexpr std::array<std::uint8_t, 28> GetUnitTypeExpected {
	0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75,
	0x1D, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C,
	0x24, 0x30, 0xE8, 0x39, 0x9E, 0xFF, 0xFF, 0x84,
	0xC0, 0x74, 0x01, 0xCC,
};

constexpr std::array<std::uint8_t, 32> GetGameFromUnitExpected {
	0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
	0xD9, 0x48, 0x85, 0xC9, 0x75, 0x20, 0x88, 0x4C,
	0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
	0x44, 0xD9, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01,
};

constexpr auto GetServerUnitExpected = std::to_array<std::uint8_t>({
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
	0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41,
	0x8B, 0xD8, 0x8B, 0xF2, 0x48, 0x8B, 0xF9, 0x48,
	0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C, 0x24, 0x38,
	0x48, 0x8D, 0x4C, 0x24, 0x38, 0xE8, 0x46, 0xD1,
	0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x83,
	0xFE, 0x05, 0x75, 0x09, 0x48, 0x8D, 0x87, 0x30,
	0x36, 0x00, 0x00, 0xEB, 0x0D, 0x44, 0x8B, 0xC3,
	0x48, 0x8B, 0xD7, 0x8B, 0xCE, 0xE8, 0xE6, 0x26,
	0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x19, 0x48,
	0x8B, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x11, 0x39,
	0x58, 0x08, 0x74, 0x0E, 0x48, 0x8B, 0x80, 0x58,
	0x01, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x75, 0xEF,
	0x33, 0xC0, 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48,
	0x8B, 0x74, 0x24, 0x40, 0x48, 0x83, 0xC4, 0x20,
	0x5F, 0xC3,
});

constexpr std::array<std::uint8_t, 64> GetPlayerDataExpected {
	0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
	0xD9, 0x48, 0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C,
	0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
	0xE4, 0x99, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01,
	0xCC, 0x83, 0x3B, 0x00, 0x74, 0x14, 0x48, 0x8D,
	0x4C, 0x24, 0x30, 0xC6, 0x44, 0x24, 0x30, 0x00,
	0xE8, 0x3B, 0xAC, 0xFF, 0xFF, 0x84, 0xC0, 0x74,
	0x01, 0xCC, 0x48, 0x8B, 0x43, 0x10, 0x48, 0x83,
};

constexpr auto GameDataLayoutWitnessExpected = std::to_array<std::uint8_t>({
	0x48, 0x89, 0x4C, 0x24, 0x30, 0x0F, 0xB6, 0x91,
	0x04, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0x48,
	0x89, 0x4C, 0x24, 0x40, 0x4D, 0x8B, 0xF9, 0x0F,
	0xB6, 0x89, 0x06, 0x01, 0x00, 0x00, 0x49, 0x8B,
	0xF0, 0xE8, 0xCA, 0x28, 0xEB, 0xFF,
});

constexpr std::array<std::uint8_t, 15> AttachSoundExpected {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
	0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20,
};

using GetQuestStateFn = std::int32_t(__fastcall*)(void* questRecord, std::int32_t quest, std::int32_t state) noexcept;
using SetQuestStateFn = void(__fastcall*)(void* questRecord, std::int32_t quest, std::int32_t state) noexcept;
using ClearQuestStateFn = void(__fastcall*)(void* questRecord, std::int32_t quest, std::int32_t state) noexcept;
using GetLocalDataContextFn = std::int32_t(__fastcall*)() noexcept;
using GetLocalPlayerFn = void*(__fastcall*)(std::int32_t context) noexcept;
using GetUnitValueFn = std::int32_t(__fastcall*)(void* unit) noexcept;
using GetGameFromUnitFn = void*(__fastcall*)(void* unit) noexcept;
using GetServerUnitFn = void*(__fastcall*)(void* game, std::int32_t unitType, std::int32_t unitId) noexcept;
using GetPlayerDataFn = void*(__fastcall*)(void* playerUnit) noexcept;
using SynchronizeQuestFlagsFn = void(__fastcall*)(void* game, void* playerUnit) noexcept;
using AttachSoundFn = void(__fastcall*)(void* unit, std::uint16_t soundEvent, void* updateUnit) noexcept;

constexpr D2RL::PluginInfo Info {
	.infoSize = D2RL::PluginInfoSize,
	.abiVersion = D2RL_PLUGIN_ABI_VERSION,
	.id = "madmike-quest-reward-reset",
	.name = "Quest Reward Reset Tokens",
	.version = "1.0.22",
	.author = "MadMike",
	.description = "Resets Akara, Charsi, and Larzuk quest rewards using guarded server quest state.",
	.flags = D2RL::PluginFlags::Client | D2RL::PluginFlags::ModScopedOnly,
};

const D2RL::PluginContext* PluginContext {};
const D2RL::ItemService* ItemService {};
const D2RL::InventoryService* InventoryService {};
const D2RL::ThreadService* ThreadService {};
const D2RL::SharedEventService* SharedEventService {};
D2RL::SharedEvents::ListenerHandle TooltipListenerHandle = D2RL::SharedEvents::InvalidHandle;

std::atomic<bool> PluginActive {};
std::atomic<bool> NativeQuestContractMatches {};
std::atomic<bool> NativeResetContractMatches {};
std::atomic<bool> NativeSoundContractMatches {};
std::atomic<bool> GameTaskQueued {};
std::atomic<std::uint64_t> PendingRequestAt {};
std::atomic<std::uint32_t> PendingItemCode {};
std::atomic<std::uint32_t> PendingItemRuntimeId {};
std::atomic<std::uint64_t> PendingPlayerHandle {};
std::atomic<std::uint64_t> HoveredItemAt {};
std::atomic<std::uint32_t> HoveredItemCode {};
std::atomic<std::uint32_t> HoveredItemRuntimeId {};
std::atomic<std::uint32_t> HoveredItemContainer {};
std::atomic<std::uint64_t> HoveredPlayerHandle {};
std::atomic<LONG> HoveredCursorX {};
std::atomic<LONG> HoveredCursorY {};
std::atomic<std::uint32_t> LastLoggedHoverRuntimeId {};
std::atomic<std::uint64_t> SuppressRightButtonUpAt {};
std::atomic<HHOOK> GameMessageHook {};
std::atomic<std::uint32_t> ActiveHookCallbacks {};
std::atomic<bool> HookInstallWarningLogged {};
HWND GameWindow {};
HMODULE GameMessageHookModule {};
std::mutex GameMessageHookMutex;

auto FindToken(std::uint32_t itemCode) noexcept -> const TokenDefinition* {
	for (const auto& token : Tokens) {
		if (token.itemCode == itemCode) {
			return &token;
		}
	}
	return nullptr;
}

auto IsReadableRange(const void* address, std::size_t size) noexcept -> bool {
	if (address == nullptr || size == 0) {
		return false;
	}
	MEMORY_BASIC_INFORMATION info {};
	if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT
		|| (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
		return false;
	}
	const auto protection = info.Protect & 0xFFU;
	const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE
		|| protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ
		|| protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
	if (!readable) {
		return false;
	}
	const auto start = reinterpret_cast<std::uintptr_t>(address);
	const auto regionStart = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
	if (start < regionStart) {
		return false;
	}
	const auto offset = start - regionStart;
	return offset <= info.RegionSize && size <= info.RegionSize - offset;
}

auto CheckRuntimeQuestContract(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	const auto functionSize = static_cast<std::uint32_t>(QuestFunctionExpected.size());
	const bool getterMatches = context->CheckExpectedBytes(GetQuestStateRva, QuestFunctionExpected.data(), functionSize);
	const bool setterMatches = context->CheckExpectedBytes(SetQuestStateRva, QuestFunctionExpected.data(), functionSize);
	const bool clearerMatches = context->CheckExpectedBytes(ClearQuestStateRva, QuestFunctionExpected.data(), functionSize);
	const bool clearCallerMatches = context->CheckExpectedBytes(
		ClearQuestCallerWitnessRva,
		ClearQuestCallerWitnessExpected.data(),
		static_cast<std::uint32_t>(ClearQuestCallerWitnessExpected.size()));
	const bool setCallerMatches = context->CheckExpectedBytes(
		SetQuestCallerWitnessRva,
		SetQuestCallerWitnessExpected.data(),
		static_cast<std::uint32_t>(SetQuestCallerWitnessExpected.size()));
	const bool matches = getterMatches && setterMatches && clearerMatches
		&& clearCallerMatches && setCallerMatches;
	NativeQuestContractMatches.store(matches, std::memory_order_release);

	char message[320] {};
	std::snprintf(message, sizeof(message),
		"Quest Reward Reset: runtime quest contract %s (Get=%s Set=%s Clear=%s clear-caller=%s set-caller=%s).",
		matches ? "matched" : "mismatch; quest functions disabled",
		getterMatches ? "match" : "mismatch",
		setterMatches ? "match" : "mismatch",
		clearerMatches ? "match" : "mismatch",
		clearCallerMatches ? "match" : "mismatch",
		setCallerMatches ? "match" : "mismatch");
	if (matches) {
		context->LogInfo(message);
	} else {
		context->LogWarn(message);
	}
	return matches;
}


auto CheckRuntimeResetContract(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		NativeResetContractMatches.store(false, std::memory_order_release);
		return false;
	}
	const bool entryMatches = context->CheckExpectedBytes(
		SynchronizeQuestFlagsRva,
		SynchronizeQuestFlagsExpected.data(),
		static_cast<std::uint32_t>(SynchronizeQuestFlagsExpected.size()));
	const bool callerOneMatches = context->CheckExpectedBytes(
		SynchronizeQuestCallerOneRva,
		SynchronizeCallerOneExpected.data(),
		static_cast<std::uint32_t>(SynchronizeCallerOneExpected.size()));
	const bool callerTwoMatches = context->CheckExpectedBytes(
		SynchronizeQuestCallerTwoRva,
		SynchronizeCallerTwoExpected.data(),
		static_cast<std::uint32_t>(SynchronizeCallerTwoExpected.size()));
	const bool callerThreeMatches = context->CheckExpectedBytes(
		SynchronizeQuestCallerThreeRva,
		SynchronizeCallerThreeExpected.data(),
		static_cast<std::uint32_t>(SynchronizeCallerThreeExpected.size()));
	const bool localContextMatches = context->CheckExpectedBytes(
		GetLocalDataContextRva,
		GetLocalDataContextExpected.data(),
		static_cast<std::uint32_t>(GetLocalDataContextExpected.size()));
	const bool localPlayerMatches = context->CheckExpectedBytes(
		GetLocalPlayerRva,
		GetLocalPlayerExpected.data(),
		static_cast<std::uint32_t>(GetLocalPlayerExpected.size()));
	const bool unitIdMatches = context->CheckExpectedBytes(
		GetUnitIdRva,
		GetUnitIdExpected.data(),
		static_cast<std::uint32_t>(GetUnitIdExpected.size()));
	const bool unitTypeMatches = context->CheckExpectedBytes(
		GetUnitTypeRva,
		GetUnitTypeExpected.data(),
		static_cast<std::uint32_t>(GetUnitTypeExpected.size()));
	const bool gameFromUnitMatches = context->CheckExpectedBytes(
		GetGameFromUnitRva,
		GetGameFromUnitExpected.data(),
		static_cast<std::uint32_t>(GetGameFromUnitExpected.size()));
	const bool serverUnitMatches = context->CheckExpectedBytes(
		GetServerUnitRva,
		GetServerUnitExpected.data(),
		static_cast<std::uint32_t>(GetServerUnitExpected.size()));
	const bool playerDataMatches = context->CheckExpectedBytes(
		GetPlayerDataRva,
		GetPlayerDataExpected.data(),
		static_cast<std::uint32_t>(GetPlayerDataExpected.size()));
	const bool gameLayoutMatches = context->CheckExpectedBytes(
		GameDataLayoutWitnessRva,
		GameDataLayoutWitnessExpected.data(),
		static_cast<std::uint32_t>(GameDataLayoutWitnessExpected.size()));
	const bool matches = entryMatches && callerOneMatches && callerTwoMatches && callerThreeMatches
		&& localContextMatches && localPlayerMatches && unitIdMatches && unitTypeMatches && gameFromUnitMatches
		&& serverUnitMatches && playerDataMatches && gameLayoutMatches;
	NativeResetContractMatches.store(matches, std::memory_order_release);
	char message[384] {};
	std::snprintf(message, sizeof(message),
		"Quest Reward Reset: authoritative reset contract %s (sync=%s callers=%s/%s/%s local=%s/%s unit-type/id=%s/%s game=%s server=%s player-data=%s layout=%s).",
		matches ? "matched" : "mismatch; reset functions disabled",
		entryMatches ? "match" : "mismatch",
		callerOneMatches ? "match" : "mismatch",
		callerTwoMatches ? "match" : "mismatch",
		callerThreeMatches ? "match" : "mismatch",
		localContextMatches ? "match" : "mismatch",
		localPlayerMatches ? "match" : "mismatch",
		unitTypeMatches ? "match" : "mismatch",
		unitIdMatches ? "match" : "mismatch",
		gameFromUnitMatches ? "match" : "mismatch",
		serverUnitMatches ? "match" : "mismatch",
		playerDataMatches ? "match" : "mismatch",
		gameLayoutMatches ? "match" : "mismatch");
	if (matches) {
		context->LogInfo(message);
	} else {
		context->LogWarn(message);
	}
	return matches;
}

auto CheckRuntimeSoundContract(const D2RL::PluginContext* context) noexcept -> bool {
	const bool matches = context != nullptr && context->CheckExpectedBytes(
		AttachSoundRva,
		AttachSoundExpected.data(),
		static_cast<std::uint32_t>(AttachSoundExpected.size()));
	NativeSoundContractMatches.store(matches, std::memory_order_release);
	if (context != nullptr) {
		if (matches) {
			context->LogInfo("Quest Reward Reset: native item-success sound contract matched; feedback sound is enabled.");
		} else {
			context->LogWarn("Quest Reward Reset: native sound contract mismatch; quest resets remain enabled without sound feedback.");
		}
	}
	return matches;
}

auto TryPlayConsumedSound(const D2RL::PluginContext* context, void* player) noexcept -> bool {
	if (context == nullptr || player == nullptr
		|| !NativeSoundContractMatches.load(std::memory_order_acquire)) {
		return false;
	}
	const auto attachSound = reinterpret_cast<AttachSoundFn>(context->exeBase + AttachSoundRva);
	__try {
		attachSound(player, ItemConsumedSoundId, player);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

enum class NativeTargetFailure : std::uint32_t {
	None,
	InvalidContext,
	InvalidDataContext,
	LocalPlayerUnavailable,
	LocalPlayerIdentityInvalid,
	ClientRegistryUnavailable,
	ClientRegistryEmpty,
	ClientRegistryCorrupt,
	ClientObjectNotFound,
	ClientObjectIdentityNotFound,
	ClientObjectAmbiguous,
	ClientObjectLayoutMismatch,
	GameUnavailable,
	GameContextMismatch,
	DifficultyInvalid,
	ServerPlayerUnavailable,
	ServerPlayerMismatch,
	PlayerDataUnavailable,
	QuestRecordSlotUnavailable,
	QuestRecordUnavailable,
	RuntimeContractUnavailable,
	NativeException,
};

auto NativeTargetFailureName(NativeTargetFailure failure) noexcept -> const char* {
	switch (failure) {
	case NativeTargetFailure::None: return "none";
	case NativeTargetFailure::InvalidContext: return "invalid-plugin-context";
	case NativeTargetFailure::InvalidDataContext: return "local-data-context-out-of-range";
	case NativeTargetFailure::LocalPlayerUnavailable: return "local-client-player-unavailable";
	case NativeTargetFailure::LocalPlayerIdentityInvalid: return "local-player-type-or-id-invalid";
	case NativeTargetFailure::ClientRegistryUnavailable: return "client-registry-unreadable";
	case NativeTargetFailure::ClientRegistryEmpty: return "client-registry-empty";
	case NativeTargetFailure::ClientRegistryCorrupt: return "client-registry-invalid-or-cyclic";
	case NativeTargetFailure::ClientObjectNotFound: return "local-client-object-not-found";
	case NativeTargetFailure::ClientObjectIdentityNotFound: return "local-client-identity-not-found";
	case NativeTargetFailure::ClientObjectAmbiguous: return "local-client-object-ambiguous";
	case NativeTargetFailure::ClientObjectLayoutMismatch: return "local-client-layout-or-identity-mismatch";
	case NativeTargetFailure::GameUnavailable: return "game-pointer-unavailable";
	case NativeTargetFailure::GameContextMismatch: return "game-client-context-mismatch";
	case NativeTargetFailure::DifficultyInvalid: return "game-difficulty-invalid";
	case NativeTargetFailure::ServerPlayerUnavailable: return "server-player-lookup-failed";
	case NativeTargetFailure::ServerPlayerMismatch: return "server-player-identity-or-game-mismatch";
	case NativeTargetFailure::PlayerDataUnavailable: return "server-player-data-unavailable";
	case NativeTargetFailure::QuestRecordSlotUnavailable: return "difficulty-quest-record-slot-unreadable";
	case NativeTargetFailure::QuestRecordUnavailable: return "difficulty-quest-record-unavailable";
	case NativeTargetFailure::RuntimeContractUnavailable: return "runtime-contract-unavailable";
	case NativeTargetFailure::NativeException: return "native-access-exception";
	}
	return "unknown";
}

struct NativeQuestTarget {
	void* game {};
	void* serverPlayer {};
	void* questRecord {};
	std::uint8_t difficulty {};
};

struct NativeClientAnchor {
	void* game {};
	std::uint8_t dataContext {};
};

auto TryResolveNativeClientAnchor(
	const D2RL::PluginContext* context,
	std::int32_t clientPlayerType,
	std::int32_t clientPlayerId,
	GetUnitValueFn getUnitId,
	GetUnitValueFn getUnitType,
	NativeClientAnchor* anchor,
	NativeTargetFailure* failure) noexcept -> bool {
	if (context == nullptr || anchor == nullptr || context->exeBase == 0) {
		if (failure != nullptr) *failure = NativeTargetFailure::InvalidContext;
		return false;
	}
	const auto* const bucketTable = reinterpret_cast<const std::uint8_t*>(context->exeBase + GlobalClientIdHashBucketsRva);
	if (!IsReadableRange(bucketTable, ClientIdHashBucketCount * sizeof(void*))) {
		if (failure != nullptr) *failure = NativeTargetFailure::ClientRegistryUnavailable;
		return false;
	}

	std::array<const void*, MaximumRegisteredClients> visited {};
	std::size_t visitedCount = 0;
	std::size_t identityMatches = 0;
	std::size_t localMatches = 0;
	const void* localClient = nullptr;
	NativeClientAnchor resolved {};
	for (std::size_t bucket = 0; bucket < ClientIdHashBucketCount; ++bucket) {
		const void* current {};
		std::memcpy(&current, bucketTable + bucket * sizeof(void*), sizeof(current));
		while (current != nullptr) {
			if (visitedCount >= visited.size()
				|| (reinterpret_cast<std::uintptr_t>(current) & 0x7U) != 0
				|| !IsReadableRange(current, ClientDataContextOffset + sizeof(std::uint8_t))) {
				if (failure != nullptr) *failure = NativeTargetFailure::ClientRegistryCorrupt;
				return false;
			}
			for (std::size_t index = 0; index < visitedCount; ++index) {
				if (visited[index] == current) {
					if (failure != nullptr) *failure = NativeTargetFailure::ClientRegistryCorrupt;
					return false;
				}
			}
			visited[visitedCount++] = current;

			const auto* const clientBytes = static_cast<const std::uint8_t*>(current);
			void* cachedPlayer {};
			std::uint32_t cachedPlayerType {};
			std::uint32_t cachedPlayerId {};
			std::uint8_t cachedDataContext {};
			std::memcpy(&cachedPlayer, clientBytes + ClientPlayerPointerOffset, sizeof(cachedPlayer));
			std::memcpy(&cachedPlayerType, clientBytes + ClientPlayerTypeOffset, sizeof(cachedPlayerType));
			std::memcpy(&cachedPlayerId, clientBytes + ClientPlayerIdOffset, sizeof(cachedPlayerId));
			std::memcpy(&cachedDataContext, clientBytes + ClientDataContextOffset, sizeof(cachedDataContext));
			if (cachedPlayerType == static_cast<std::uint32_t>(clientPlayerType)
				&& cachedPlayerId == static_cast<std::uint32_t>(clientPlayerId)) {
				++identityMatches;
				if (cachedPlayer == nullptr || (reinterpret_cast<std::uintptr_t>(cachedPlayer) & 0x7U) != 0
					|| !IsReadableRange(cachedPlayer, 0x10)
					|| getUnitType(cachedPlayer) != clientPlayerType || getUnitId(cachedPlayer) != clientPlayerId) {
					if (failure != nullptr) *failure = NativeTargetFailure::ClientObjectLayoutMismatch;
					return false;
				}
				void* game {};
				std::memcpy(&game, clientBytes + ClientGamePointerOffset, sizeof(game));
				++localMatches;
				localClient = current;
				resolved.game = game;
				resolved.dataContext = cachedDataContext;
			}

			const void* next {};
			std::memcpy(&next, clientBytes + ClientNextOffset, sizeof(next));
			current = next;
		}
	}
	if (visitedCount == 0) {
		if (failure != nullptr) *failure = NativeTargetFailure::ClientRegistryEmpty;
		return false;
	}
	if (identityMatches == 0) {
		if (failure != nullptr) *failure = NativeTargetFailure::ClientObjectIdentityNotFound;
		return false;
	}
	if (localMatches == 0 || localClient == nullptr) {
		if (failure != nullptr) *failure = NativeTargetFailure::ClientObjectNotFound;
		return false;
	}
	if (localMatches != 1) {
		if (failure != nullptr) *failure = NativeTargetFailure::ClientObjectAmbiguous;
		return false;
	}
	if (resolved.game == nullptr || (reinterpret_cast<std::uintptr_t>(resolved.game) & 0x7U) != 0
		|| !IsReadableRange(resolved.game, GameRequiredReadableSize)) {
		if (failure != nullptr) *failure = NativeTargetFailure::GameUnavailable;
		return false;
	}
	std::uint8_t gameDataContext {};
	std::memcpy(&gameDataContext,
		static_cast<const std::uint8_t*>(resolved.game) + GameDataContextOffset,
		sizeof(gameDataContext));
	if (gameDataContext != resolved.dataContext) {
		if (failure != nullptr) *failure = NativeTargetFailure::GameContextMismatch;
		return false;
	}
	*anchor = resolved;
	if (failure != nullptr) *failure = NativeTargetFailure::None;
	return true;
}

auto TryResolveNativeQuestTargetUnsafe(
	const D2RL::PluginContext* context,
	NativeQuestTarget* target,
	NativeTargetFailure* failure) noexcept -> bool {
	if (context == nullptr || target == nullptr || context->exeBase == 0) {
		if (failure != nullptr) *failure = NativeTargetFailure::InvalidContext;
		return false;
	}
	const auto getLocalDataContext = reinterpret_cast<GetLocalDataContextFn>(context->exeBase + GetLocalDataContextRva);
	const auto getLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(context->exeBase + GetLocalPlayerRva);
	const auto getUnitId = reinterpret_cast<GetUnitValueFn>(context->exeBase + GetUnitIdRva);
	const auto getUnitType = reinterpret_cast<GetUnitValueFn>(context->exeBase + GetUnitTypeRva);
	const auto getGameFromUnit = reinterpret_cast<GetGameFromUnitFn>(context->exeBase + GetGameFromUnitRva);
	const auto getServerUnit = reinterpret_cast<GetServerUnitFn>(context->exeBase + GetServerUnitRva);
	const auto getPlayerData = reinterpret_cast<GetPlayerDataFn>(context->exeBase + GetPlayerDataRva);
	const auto dataContext = getLocalDataContext();
	if (dataContext < 0 || dataContext >= 8) {
		if (failure != nullptr) *failure = NativeTargetFailure::InvalidDataContext;
		return false;
	}
	void* const clientPlayer = getLocalPlayer(dataContext);
	if (clientPlayer == nullptr || (reinterpret_cast<std::uintptr_t>(clientPlayer) & 0x7U) != 0
		|| !IsReadableRange(clientPlayer, 0x10)) {
		if (failure != nullptr) *failure = NativeTargetFailure::LocalPlayerUnavailable;
		return false;
	}
	const auto clientPlayerType = getUnitType(clientPlayer);
	const auto clientPlayerId = getUnitId(clientPlayer);
	if (clientPlayerType != PlayerUnitType || clientPlayerId < 0) {
		if (failure != nullptr) *failure = NativeTargetFailure::LocalPlayerIdentityInvalid;
		return false;
	}
	NativeClientAnchor clientAnchor {};
	if (!TryResolveNativeClientAnchor(
			context,
			clientPlayerType,
			clientPlayerId,
			getUnitId,
			getUnitType,
			&clientAnchor,
			failure)) {
		return false;
	}
	void* const game = clientAnchor.game;
	if (!IsReadableRange(static_cast<const std::uint8_t*>(game) + GameDifficultyOffset, 1)) {
		if (failure != nullptr) *failure = NativeTargetFailure::GameUnavailable;
		return false;
	}
	std::uint8_t difficulty {};
	std::memcpy(&difficulty, static_cast<const std::uint8_t*>(game) + GameDifficultyOffset, sizeof(difficulty));
	if (difficulty > 2) {
		if (failure != nullptr) *failure = NativeTargetFailure::DifficultyInvalid;
		return false;
	}
	void* const serverPlayer = getServerUnit(game, PlayerUnitType, clientPlayerId);
	if (serverPlayer == nullptr || (reinterpret_cast<std::uintptr_t>(serverPlayer) & 0x7U) != 0
		|| !IsReadableRange(serverPlayer, 0x10)) {
		if (failure != nullptr) *failure = NativeTargetFailure::ServerPlayerUnavailable;
		return false;
	}
	const auto serverPlayerType = getUnitType(serverPlayer);
	const auto serverPlayerId = getUnitId(serverPlayer);
	if (serverPlayerType != PlayerUnitType || serverPlayerId != clientPlayerId
		|| getGameFromUnit(serverPlayer) != game) {
		if (failure != nullptr) *failure = NativeTargetFailure::ServerPlayerMismatch;
		return false;
	}
	void* const playerData = getPlayerData(serverPlayer);
	if (playerData == nullptr || (reinterpret_cast<std::uintptr_t>(playerData) & 0x7U) != 0) {
		if (failure != nullptr) *failure = NativeTargetFailure::PlayerDataUnavailable;
		return false;
	}
	const auto questRecordOffset = PlayerDataQuestRecordArrayOffset
		+ static_cast<std::size_t>(difficulty) * sizeof(void*);
	const auto* const questRecordSlot = static_cast<const std::uint8_t*>(playerData) + questRecordOffset;
	if (!IsReadableRange(questRecordSlot, sizeof(void*))) {
		if (failure != nullptr) *failure = NativeTargetFailure::QuestRecordSlotUnavailable;
		return false;
	}
	void* questRecord {};
	std::memcpy(&questRecord, questRecordSlot, sizeof(questRecord));
	if (questRecord == nullptr || (reinterpret_cast<std::uintptr_t>(questRecord) & 0x7U) != 0
		|| !IsReadableRange(questRecord, 0x20)) {
		if (failure != nullptr) *failure = NativeTargetFailure::QuestRecordUnavailable;
		return false;
	}
	target->game = game;
	target->serverPlayer = serverPlayer;
	target->questRecord = questRecord;
	target->difficulty = difficulty;
	if (failure != nullptr) *failure = NativeTargetFailure::None;
	return true;
}

auto TryResolveNativeQuestTarget(
	const D2RL::PluginContext* context,
	NativeQuestTarget* target,
	NativeTargetFailure* failure) noexcept -> bool {
	if (!NativeResetContractMatches.load(std::memory_order_acquire)) {
		if (failure != nullptr) *failure = NativeTargetFailure::RuntimeContractUnavailable;
		return false;
	}
	__try {
		return TryResolveNativeQuestTargetUnsafe(context, target, failure);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		if (failure != nullptr) *failure = NativeTargetFailure::NativeException;
		return false;
	}
}

auto TrySynchronizeQuestFlags(
	const D2RL::PluginContext* context,
	void* game,
	void* player) noexcept -> bool {
	if (context == nullptr || game == nullptr || player == nullptr
		|| !NativeResetContractMatches.load(std::memory_order_acquire)) {
		return false;
	}
	const auto synchronize = reinterpret_cast<SynchronizeQuestFlagsFn>(context->exeBase + SynchronizeQuestFlagsRva);
	__try {
		synchronize(game, player);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

auto IsCurrentProcessWindow(HWND window) noexcept -> bool {
	if (window == nullptr || !IsWindow(window) || !IsWindowVisible(window)) {
		return false;
	}
	DWORD processId {};
	GetWindowThreadProcessId(window, &processId);
	return processId == GetCurrentProcessId();
}

BOOL CALLBACK FindGameWindowCallback(HWND window, LPARAM state) noexcept {
	if (!IsCurrentProcessWindow(window) || GetWindow(window, GW_OWNER) != nullptr) {
		return TRUE;
	}
	*reinterpret_cast<HWND*>(state) = window;
	return FALSE;
}

auto FindGameWindow() noexcept -> HWND {
	const auto foreground = GetForegroundWindow();
	if (IsCurrentProcessWindow(foreground)) {
		return foreground;
	}
	HWND window {};
	EnumWindows(FindGameWindowCallback, reinterpret_cast<LPARAM>(&window));
	return window;
}

LRESULT CALLBACK GameMessageHookCallback(int code, WPARAM removeMode, LPARAM parameter) noexcept;

void LogHookInstallFailureOnce() noexcept {
	if (PluginContext != nullptr && !HookInstallWarningLogged.exchange(true, std::memory_order_acq_rel)) {
		PluginContext->LogWarn("Quest Reward Reset: could not install the game-window right-click hook; it will retry when a token tooltip appears.");
	}
}

auto TryInstallGameMessageHook() noexcept -> bool {
	if (GameMessageHook.load(std::memory_order_acquire) != nullptr) {
		return true;
	}
	const std::lock_guard lock(GameMessageHookMutex);
	if (GameMessageHook.load(std::memory_order_relaxed) != nullptr) {
		return true;
	}

	const auto window = FindGameWindow();
	if (window == nullptr) {
		LogHookInstallFailureOnce();
		return false;
	}
	const auto threadId = GetWindowThreadProcessId(window, nullptr);
	if (threadId == 0) {
		LogHookInstallFailureOnce();
		return false;
	}

	HMODULE module {};
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<LPCWSTR>(&GameMessageHookCallback),
			&module)) {
		LogHookInstallFailureOnce();
		return false;
	}
	const auto hook = SetWindowsHookExW(WH_GETMESSAGE, GameMessageHookCallback, module, threadId);
	if (hook == nullptr) {
		FreeLibrary(module);
		LogHookInstallFailureOnce();
		return false;
	}
	GameWindow = window;
	GameMessageHookModule = module;
	GameMessageHook.store(hook, std::memory_order_release);
	HookInstallWarningLogged.store(false, std::memory_order_release);
	if (PluginContext != nullptr) {
		PluginContext->LogInfo("Quest Reward Reset: right-click handler installed for the active D2R window.");
	}
	return true;
}

struct TokenSearch {
	std::uint32_t itemCode {};
	std::uint32_t runtimeId {};
	D2RL::ItemHandle handle { D2RL::InvalidItemHandle };
	std::int32_t quantity {};
};

auto __cdecl FindTokenInInventory(
	const D2RL::PluginContext*,
	const D2RL::Items::ItemInfo* item,
	void* userData) noexcept -> D2RL::Inventory::IterationAction {
	using D2RL::Inventory::IterationAction;
	auto* search = static_cast<TokenSearch*>(userData);
	if (search == nullptr || item == nullptr || item->structSize < D2RL::Items::ItemInfoRequiredSize) {
		return IterationAction::Continue;
	}
	if (item->runtimeId == search->runtimeId && item->code == search->itemCode
		&& (item->container == D2RL::Items::ItemContainer::Inventory
			|| item->container == D2RL::Items::ItemContainer::Cube)) {
		search->handle = item->handle;
		search->quantity = item->quantity;
		return IterationAction::Stop;
	}
	return IterationAction::Continue;
}

auto FindCurrentToken(
	const D2RL::PluginContext* context,
	D2RL::PlayerHandle player,
	std::uint32_t itemCode,
	std::uint32_t runtimeId,
	TokenSearch& search) noexcept -> bool {
	search.itemCode = itemCode;
	search.runtimeId = runtimeId;
	const D2RL::Inventory::ItemFilter filter {
		.structSize = D2RL::Inventory::ItemFilterSize,
		.containerMask = D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory)
			| D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Cube),
	};
	const auto result = InventoryService->forEachInventoryItem(context, player, &filter, FindTokenInInventory, &search);
	return result == D2RL::Inventory::Result::Success && search.handle != D2RL::InvalidItemHandle && search.quantity > 0;
}

auto ConsumeOneToken(
	const D2RL::PluginContext* context,
	D2RL::PlayerHandle player,
	D2RL::ItemHandle item,
	std::int32_t quantity) noexcept -> D2RL::Items::Result {
	if (quantity == 1) {
		return ItemService->destroyItem(
			context,
			player,
			item,
			D2RL::Items::SocketedItemPolicy::RejectIfNotEmpty);
	}

	const D2RL::Items::ExistingItemOperation operation {
		.structSize = sizeof(D2RL::Items::ExistingItemOperation),
		.kind = D2RL::Items::ExistingItemOperationKind::Debit,
		.item = item,
		.debit = { .quantity = 1 },
	};
	const D2RL::Items::ExistingItemTransaction transaction {
		.structSize = sizeof(D2RL::Items::ExistingItemTransaction),
		.player = player,
		.operationCount = 1,
		.operations = &operation,
	};
	D2RL::Items::ExistingItemTransactionResult result {
		.structSize = sizeof(D2RL::Items::ExistingItemTransactionResult),
	};
	if (ItemService->executeExistingItemTransaction(context, &transaction, &result) != D2RL::Items::Result::Success
		|| result.committedOperationCount != 1) {
		return D2RL::Items::Result::Conflict;
	}
	return D2RL::Items::Result::Success;
}

void __cdecl ProcessTokenUse(const D2RL::PluginContext* context, void*) noexcept {
	struct CompletionGuard {
		~CompletionGuard() noexcept {
			PendingItemCode.store(0, std::memory_order_release);
			PendingItemRuntimeId.store(0, std::memory_order_release);
			PendingPlayerHandle.store(0, std::memory_order_release);
			PendingRequestAt.store(0, std::memory_order_release);
			GameTaskQueued.store(false, std::memory_order_release);
		}
	} completionGuard;

	if (!PluginActive.load(std::memory_order_acquire) || context == nullptr) {
		return;
	}
	const auto requestAt = PendingRequestAt.load(std::memory_order_acquire);
	const auto now = GetTickCount64();
	if (requestAt == 0 || now < requestAt || now - requestAt > PendingRequestLifetimeMilliseconds) {
		context->LogWarn("Quest Reward Reset: right-click request expired before the game-thread callback ran.");
		return;
	}
	const auto itemCode = PendingItemCode.load(std::memory_order_acquire);
	const auto runtimeId = PendingItemRuntimeId.load(std::memory_order_acquire);
	const auto* const token = FindToken(itemCode);
	if (token == nullptr || runtimeId == 0) {
		context->LogWarn("Quest Reward Reset: queued token identity was invalid; no quest flags were changed.");
		return;
	}
	if (!NativeQuestContractMatches.load(std::memory_order_acquire)
		|| !CheckRuntimeQuestContract(context)
		|| !NativeResetContractMatches.load(std::memory_order_acquire)
		|| !CheckRuntimeResetContract(context)) {
		context->LogWarn("Quest Reward Reset: quest runtime contract changed; no quest functions were called.");
		return;
	}

	const auto player = PendingPlayerHandle.load(std::memory_order_acquire);
	if (player == D2RL::InvalidPlayerHandle) {
		context->LogWarn("Quest Reward Reset: cached local-player handle is unavailable; token was kept.");
		return;
	}
	TokenSearch search {};
	if (!FindCurrentToken(context, player, itemCode, runtimeId, search)) {
		context->LogWarn("Quest Reward Reset: clicked token is no longer in the inventory or Cube; token was not consumed.");
		return;
	}

	NativeQuestTarget target {};
	NativeTargetFailure targetFailure = NativeTargetFailure::None;
	if (!TryResolveNativeQuestTarget(context, &target, &targetFailure)) {
		char message[224] {};
		std::snprintf(message, sizeof(message),
			"Quest Reward Reset: authoritative current-difficulty quest record lookup failed at %s; token was kept.",
			NativeTargetFailureName(targetFailure));
		context->LogWarn(message);
		return;
	}
	const auto getQuestState = reinterpret_cast<GetQuestStateFn>(context->exeBase + GetQuestStateRva);
	const auto setQuestState = reinterpret_cast<SetQuestStateFn>(context->exeBase + SetQuestStateRva);
	const auto clearQuestState = reinterpret_cast<ClearQuestStateFn>(context->exeBase + ClearQuestStateRva);
	const auto grantedBefore = getQuestState(target.questRecord, token->questId, RewardGrantedFlag);
	const auto pendingBefore = getQuestState(target.questRecord, token->questId, RewardPendingFlag);
	if (grantedBefore == 0 && pendingBefore != 0) {
		char message[192] {};
		std::snprintf(message, sizeof(message), "Quest Reward Reset: %s reward is already claimable in the current difficulty; token was kept.", token->name);
		context->LogInfo(message);
		return;
	}
	if (grantedBefore == 0 || pendingBefore != 0) {
		char message[224] {};
		std::snprintf(message, sizeof(message),
			"Quest Reward Reset: %s is not in a consumed-and-resettable state (granted=%d pending=%d); token was kept.",
			token->name, grantedBefore, pendingBefore);
		context->LogWarn(message);
		return;
	}

	clearQuestState(target.questRecord, token->questId, RewardGrantedFlag);
	setQuestState(target.questRecord, token->questId, RewardPendingFlag);
	const auto grantedAfter = getQuestState(target.questRecord, token->questId, RewardGrantedFlag);
	const auto pendingAfter = getQuestState(target.questRecord, token->questId, RewardPendingFlag);
	if (grantedAfter != 0 || pendingAfter == 0) {
		char message[224] {};
		std::snprintf(message, sizeof(message),
			"Quest Reward Reset: %s flag readback failed (granted=%d pending=%d); token was kept.",
			token->name, grantedAfter, pendingAfter);
		context->LogWarn(message);
		return;
	}
	if (!TrySynchronizeQuestFlags(context, target.game, target.serverPlayer)) {
		char message[224] {};
		std::snprintf(message, sizeof(message),
			"Quest Reward Reset: %s flags were reset on the server, but synchronization failed; token was kept.",
			token->name);
		context->LogWarn(message);
		return;
	}
	const auto grantedSynced = getQuestState(target.questRecord, token->questId, RewardGrantedFlag);
	const auto pendingSynced = getQuestState(target.questRecord, token->questId, RewardPendingFlag);
	if (grantedSynced != 0 || pendingSynced == 0) {
		char message[224] {};
		std::snprintf(message, sizeof(message),
			"Quest Reward Reset: %s server-state readback failed after synchronization (granted=%d pending=%d); token was kept.",
			token->name, grantedSynced, pendingSynced);
		context->LogWarn(message);
		return;
	}

	const auto consumeResult = ConsumeOneToken(context, player, search.handle, search.quantity);
	if (consumeResult != D2RL::Items::Result::Success) {
		char message[256] {};
		std::snprintf(message, sizeof(message),
			"Quest Reward Reset: %s flags reset in the current difficulty, but the token could not be consumed (item result=%u).",
			token->name, static_cast<std::uint32_t>(consumeResult));
		context->LogWarn(message);
		return;
	}
	if (!TryPlayConsumedSound(context, target.serverPlayer)) {
		context->LogWarn("Quest Reward Reset: reward reset and token consumed, but the success sound could not be played.");
	}

	char message[224] {};
	std::snprintf(message, sizeof(message), "%s reward reset and synchronized in the current difficulty (%u); one token consumed.",
		token->name, static_cast<unsigned>(target.difficulty));
	context->LogInfo(message);
}

auto QueueTokenUse(std::uint32_t itemCode, std::uint32_t runtimeId, D2RL::PlayerHandle player) noexcept -> bool {
	const auto* const context = PluginContext;
	const auto* const token = FindToken(itemCode);
	if (context == nullptr || token == nullptr) {
		return false;
	}
	if (ThreadService == nullptr || ThreadService->runOnGameThread == nullptr
		|| !NativeQuestContractMatches.load(std::memory_order_acquire)
		|| !NativeResetContractMatches.load(std::memory_order_acquire)) {
		context->LogWarn("Quest Reward Reset: native reset contract or game-thread service is unavailable; token activation was suppressed and the token was kept.");
		return true;
	}
	const auto now = GetTickCount64();
	if (GameTaskQueued.load(std::memory_order_acquire)) {
		const auto queuedAt = PendingRequestAt.load(std::memory_order_acquire);
		if (queuedAt != 0 && now >= queuedAt && now - queuedAt > PendingRequestLifetimeMilliseconds) {
			bool expected = true;
			if (GameTaskQueued.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
				context->LogWarn("Quest Reward Reset: discarded an expired queued token request.");
			}
		}
	}
	if (GameTaskQueued.exchange(true, std::memory_order_acq_rel)) {
		context->LogInfo("Quest Reward Reset: another reset is already queued; this token activation was suppressed and the token was kept.");
		return true;
	}
	PendingItemCode.store(itemCode, std::memory_order_release);
	PendingItemRuntimeId.store(runtimeId, std::memory_order_release);
	PendingPlayerHandle.store(player, std::memory_order_release);
	PendingRequestAt.store(now, std::memory_order_release);
	const auto result = ThreadService->runOnGameThread(context, ProcessTokenUse, nullptr);
	if (result != D2RL::Threads::Result::Success) {
		PendingItemCode.store(0, std::memory_order_release);
		PendingItemRuntimeId.store(0, std::memory_order_release);
		PendingPlayerHandle.store(0, std::memory_order_release);
		PendingRequestAt.store(0, std::memory_order_release);
		GameTaskQueued.store(false, std::memory_order_release);
		char message[192] {};
		std::snprintf(message, sizeof(message),
			"Quest Reward Reset: could not queue the reset on the authoritative game thread (result=%u); token activation was suppressed and the token was kept.",
			static_cast<std::uint32_t>(result));
		context->LogWarn(message);
		return true;
	}
	return true;
}

LRESULT CALLBACK GameMessageHookCallback(int code, WPARAM removeMode, LPARAM parameter) noexcept {
	struct CallbackGuard {
		CallbackGuard() noexcept {
			ActiveHookCallbacks.fetch_add(1, std::memory_order_acq_rel);
		}
		~CallbackGuard() noexcept {
			ActiveHookCallbacks.fetch_sub(1, std::memory_order_acq_rel);
		}
	} callbackGuard;

	auto* message = code >= 0 && removeMode == PM_REMOVE && parameter != 0
		? reinterpret_cast<MSG*>(parameter)
		: nullptr;
	if (message == nullptr || !PluginActive.load(std::memory_order_acquire)
		|| (GameWindow != nullptr && message->hwnd != GameWindow)) {
		return CallNextHookEx(GameMessageHook.load(std::memory_order_acquire), code, removeMode, parameter);
	}

	if (message->message == WM_RBUTTONUP) {
		const auto releaseAt = SuppressRightButtonUpAt.exchange(0, std::memory_order_acq_rel);
		const auto now = GetTickCount64();
		if (releaseAt != 0 && now >= releaseAt && now - releaseAt <= RightButtonReleaseLifetimeMilliseconds) {
			message->message = WM_NULL;
			return CallNextHookEx(GameMessageHook.load(std::memory_order_acquire), code, removeMode, parameter);
		}
	}

	if (message->message == WM_RBUTTONDOWN) {
		const auto hoveredAt = HoveredItemAt.load(std::memory_order_acquire);
		const auto now = GetTickCount64();
		const auto age = now >= hoveredAt ? now - hoveredAt : (std::numeric_limits<std::uint64_t>::max)();
		const auto itemCode = HoveredItemCode.load(std::memory_order_acquire);
		const auto runtimeId = HoveredItemRuntimeId.load(std::memory_order_acquire);
		const auto player = HoveredPlayerHandle.load(std::memory_order_acquire);
		const auto container = static_cast<D2RL::Items::ItemContainer>(HoveredItemContainer.load(std::memory_order_acquire));
		POINT clickPosition {
			GET_X_LPARAM(message->lParam),
			GET_Y_LPARAM(message->lParam),
		};
		const bool clickPositionKnown = ClientToScreen(message->hwnd, &clickPosition) != FALSE;
		const auto hoverX = HoveredCursorX.load(std::memory_order_acquire);
		const auto hoverY = HoveredCursorY.load(std::memory_order_acquire);
		const auto dx = static_cast<std::int64_t>(clickPosition.x) - hoverX;
		const auto dy = static_cast<std::int64_t>(clickPosition.y) - hoverY;
		if (hoveredAt != 0 && age <= HoverLifetimeMilliseconds && runtimeId != 0 && player != D2RL::InvalidPlayerHandle
			&& clickPositionKnown && dx >= -2 && dx <= 2 && dy >= -2 && dy <= 2
			&& FindToken(itemCode) != nullptr
			&& (container == D2RL::Items::ItemContainer::Inventory || container == D2RL::Items::ItemContainer::Cube)
			&& QueueTokenUse(itemCode, runtimeId, player)) {
			message->message = WM_NULL;
			SuppressRightButtonUpAt.store(now, std::memory_order_release);
			const auto* const token = FindToken(itemCode);
			char log[192] {};
			std::snprintf(log, sizeof(log), "Quest Reward Reset: right-click on %s queued; native item activation suppressed.", token->name);
			if (PluginContext != nullptr) {
				PluginContext->LogInfo(log);
			}
		}
	}
	return CallNextHookEx(GameMessageHook.load(std::memory_order_acquire), code, removeMode, parameter);
}

void __cdecl OnItemTooltip(
	const D2RL::PluginContext* context,
	D2RL::SharedEvents::ItemTooltipEvent* event,
	void*) noexcept {
	if (!PluginActive.load(std::memory_order_acquire) || context == nullptr || event == nullptr
		|| event->structSize < D2RL::SharedEvents::ItemTooltipEventRequiredSize
		|| ItemService == nullptr || ItemService->getItemInfo == nullptr) {
		return;
	}

	D2RL::Items::ItemInfo itemInfo {};
	itemInfo.structSize = D2RL::Items::ItemInfoSize;
	const auto result = ItemService->getItemInfo(context, event->item, &itemInfo);
	const auto* const token = result == D2RL::Items::Result::Success && itemInfo.handle == event->item
		? FindToken(itemInfo.code) : nullptr;
	if (token == nullptr) {
		HoveredItemAt.store(0, std::memory_order_release);
		HoveredItemCode.store(0, std::memory_order_release);
		HoveredItemRuntimeId.store(0, std::memory_order_release);
		HoveredItemContainer.store(0, std::memory_order_release);
		HoveredPlayerHandle.store(0, std::memory_order_release);
		HoveredCursorX.store(0, std::memory_order_release);
		HoveredCursorY.store(0, std::memory_order_release);
		LastLoggedHoverRuntimeId.store(0, std::memory_order_release);
		return;
	}
	if (event->text != nullptr && event->capacity != 0) {
		const auto tooltipLength = std::strlen(token->tooltip);
		if (tooltipLength < event->capacity) {
			std::memcpy(event->text, token->tooltip, tooltipLength);
			event->text[tooltipLength] = '\0';
			event->length = static_cast<std::uint32_t>(tooltipLength);
		}
	}
	if (itemInfo.container != D2RL::Items::ItemContainer::Inventory
		&& itemInfo.container != D2RL::Items::ItemContainer::Cube) {
		HoveredItemAt.store(0, std::memory_order_release);
		HoveredItemCode.store(0, std::memory_order_release);
		HoveredItemRuntimeId.store(0, std::memory_order_release);
		HoveredItemContainer.store(0, std::memory_order_release);
		HoveredPlayerHandle.store(0, std::memory_order_release);
		HoveredCursorX.store(0, std::memory_order_release);
		HoveredCursorY.store(0, std::memory_order_release);
		LastLoggedHoverRuntimeId.store(0, std::memory_order_release);
		return;
	}
	TryInstallGameMessageHook();
	D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
	if (InventoryService->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success
		|| player == D2RL::InvalidPlayerHandle) {
		HoveredItemAt.store(0, std::memory_order_release);
		HoveredItemCode.store(0, std::memory_order_release);
		HoveredItemRuntimeId.store(0, std::memory_order_release);
		HoveredItemContainer.store(0, std::memory_order_release);
		HoveredPlayerHandle.store(0, std::memory_order_release);
		HoveredCursorX.store(0, std::memory_order_release);
		HoveredCursorY.store(0, std::memory_order_release);
		return;
	}
	POINT cursorPosition {};
	if (GetCursorPos(&cursorPosition) == FALSE) {
		HoveredItemAt.store(0, std::memory_order_release);
		HoveredItemCode.store(0, std::memory_order_release);
		HoveredItemRuntimeId.store(0, std::memory_order_release);
		HoveredItemContainer.store(0, std::memory_order_release);
		HoveredPlayerHandle.store(0, std::memory_order_release);
		HoveredCursorX.store(0, std::memory_order_release);
		HoveredCursorY.store(0, std::memory_order_release);
		return;
	}

	HoveredItemCode.store(itemInfo.code, std::memory_order_release);
	HoveredItemRuntimeId.store(itemInfo.runtimeId, std::memory_order_release);
	HoveredItemContainer.store(static_cast<std::uint32_t>(itemInfo.container), std::memory_order_release);
	HoveredPlayerHandle.store(player, std::memory_order_release);
	HoveredCursorX.store(cursorPosition.x, std::memory_order_release);
	HoveredCursorY.store(cursorPosition.y, std::memory_order_release);
	HoveredItemAt.store(GetTickCount64(), std::memory_order_release);
	const auto priorRuntimeId = LastLoggedHoverRuntimeId.exchange(itemInfo.runtimeId, std::memory_order_acq_rel);
	if (priorRuntimeId != itemInfo.runtimeId) {
		char message[192] {};
		std::snprintf(message, sizeof(message), "Quest Reward Reset: ready: %s token runtimeId=%u.", token->name, itemInfo.runtimeId);
		context->LogInfo(message);
	}
}

void UnregisterTooltipListener() noexcept {
	if (PluginContext != nullptr && SharedEventService != nullptr
		&& SharedEventService->unregisterItemTooltipListener != nullptr
		&& TooltipListenerHandle != D2RL::SharedEvents::InvalidHandle) {
		SharedEventService->unregisterItemTooltipListener(PluginContext, TooltipListenerHandle);
	}
	TooltipListenerHandle = D2RL::SharedEvents::InvalidHandle;
}

void RemoveGameMessageHook() noexcept {
	const std::lock_guard lock(GameMessageHookMutex);
	const auto hook = GameMessageHook.exchange(nullptr, std::memory_order_acq_rel);
	if (hook == nullptr) {
		return;
	}
	if (!UnhookWindowsHookEx(hook)) {
		if (PluginContext != nullptr) {
			PluginContext->LogWarn("Quest Reward Reset: could not remove the game-window mouse hook; retaining its module reference for safety.");
		}
		GameMessageHookModule = nullptr;
		GameWindow = nullptr;
		return;
	}
	for (std::uint32_t attempt = 0; attempt < 1000 && ActiveHookCallbacks.load(std::memory_order_acquire) != 0; ++attempt) {
		Sleep(1);
	}
	if (ActiveHookCallbacks.load(std::memory_order_acquire) == 0 && GameMessageHookModule != nullptr) {
		FreeLibrary(GameMessageHookModule);
	} else if (PluginContext != nullptr) {
		PluginContext->LogWarn("Quest Reward Reset: mouse-hook callback is still active; retaining its module reference for safety.");
	}
	GameMessageHookModule = nullptr;
	GameWindow = nullptr;
}

} // namespace QuestRewardReset

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &QuestRewardReset::Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	using namespace QuestRewardReset;
	if (context == nullptr || !D2RL::HasContext(context) || context->loadScope != D2RL::LoadScope::Mod) {
		return false;
	}

	PluginContext = context;
	RunFirstRunInstall(context);
	PluginActive.store(false, std::memory_order_release);
	NativeQuestContractMatches.store(false, std::memory_order_release);
	NativeResetContractMatches.store(false, std::memory_order_release);
	NativeSoundContractMatches.store(false, std::memory_order_release);
	GameTaskQueued.store(false, std::memory_order_release);
	PendingItemCode.store(0, std::memory_order_release);
	PendingItemRuntimeId.store(0, std::memory_order_release);
	PendingPlayerHandle.store(0, std::memory_order_release);
	PendingRequestAt.store(0, std::memory_order_release);
	HoveredItemAt.store(0, std::memory_order_release);
	HoveredItemCode.store(0, std::memory_order_release);
	HoveredItemRuntimeId.store(0, std::memory_order_release);
	HoveredItemContainer.store(0, std::memory_order_release);
	HoveredPlayerHandle.store(0, std::memory_order_release);
	HoveredCursorX.store(0, std::memory_order_release);
	HoveredCursorY.store(0, std::memory_order_release);
	LastLoggedHoverRuntimeId.store(0, std::memory_order_release);
	SuppressRightButtonUpAt.store(0, std::memory_order_release);
	HookInstallWarningLogged.store(false, std::memory_order_release);
	TooltipListenerHandle = D2RL::SharedEvents::InvalidHandle;

	if (context->QueryService(&ItemService) != D2RL::ServiceQueryResult::Success
		|| !D2RL::HasItemServiceField(ItemService, D2RL::ItemServiceRequiredSize)
		|| ItemService->getItemInfo == nullptr || ItemService->destroyItem == nullptr
		|| ItemService->executeExistingItemTransaction == nullptr
		|| context->QueryService(&InventoryService) != D2RL::ServiceQueryResult::Success
		|| !D2RL::HasInventoryServiceField(InventoryService, D2RL::InventoryServiceRequiredSize)
		|| InventoryService->getLocalPlayer == nullptr || InventoryService->forEachInventoryItem == nullptr
		|| context->QueryService(&ThreadService) != D2RL::ServiceQueryResult::Success
		|| !D2RL::HasThreadServiceField(ThreadService, D2RL::ThreadServiceRequiredSize)
		|| ThreadService->runOnGameThread == nullptr
		|| context->QueryService(&SharedEventService) != D2RL::ServiceQueryResult::Success
		|| !D2RL::HasSharedEventServiceField(SharedEventService, D2RL::SharedEventServiceRequiredSize)
		|| SharedEventService->registerItemTooltipListener == nullptr
		|| SharedEventService->unregisterItemTooltipListener == nullptr) {
		context->LogError("Quest Reward Reset: required item, inventory, thread, or tooltip service is unavailable.");
		PluginContext = nullptr;
		return false;
	}

	CheckRuntimeQuestContract(context);
	CheckRuntimeResetContract(context);
	CheckRuntimeSoundContract(context);

	const D2RL::SharedEvents::ItemTooltipListener tooltipListener {
		.structSize = D2RL::SharedEvents::ItemTooltipListenerRequiredSize,
		.priority = 0,
		.slot = 100,
		.region = D2RL::SharedEvents::ItemTooltipRegion::ActionFooter,
		.position = D2RL::SharedEvents::ItemTooltipPosition::Bottom,
		.anchor = D2RL::SharedEvents::ItemTooltipAnchor::None,
		.fallback = D2RL::SharedEvents::ItemTooltipFallback::Omit,
		.callback = OnItemTooltip,
	};
	if (SharedEventService->registerItemTooltipListener(context, &tooltipListener, &TooltipListenerHandle)
		!= D2RL::SharedEvents::Result::Success) {
		context->LogError("Quest Reward Reset: token tooltip listener registration failed.");
		TooltipListenerHandle = D2RL::SharedEvents::InvalidHandle;
		PluginContext = nullptr;
		return false;
	}

	PluginActive.store(true, std::memory_order_release);
	TryInstallGameMessageHook();
	context->LogInfo("Quest Reward Reset Tokens 1.0.22 loaded; successful current-difficulty reward resets play item-success feedback after consuming one token.");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	using namespace QuestRewardReset;
	PluginActive.store(false, std::memory_order_release);
	NativeQuestContractMatches.store(false, std::memory_order_release);
	NativeResetContractMatches.store(false, std::memory_order_release);
	NativeSoundContractMatches.store(false, std::memory_order_release);
	UnregisterTooltipListener();
	RemoveGameMessageHook();
	GameTaskQueued.store(false, std::memory_order_release);
	PendingItemCode.store(0, std::memory_order_release);
	PendingItemRuntimeId.store(0, std::memory_order_release);
	PendingPlayerHandle.store(0, std::memory_order_release);
	PendingRequestAt.store(0, std::memory_order_release);
	HoveredItemAt.store(0, std::memory_order_release);
	HoveredItemCode.store(0, std::memory_order_release);
	HoveredItemRuntimeId.store(0, std::memory_order_release);
	HoveredItemContainer.store(0, std::memory_order_release);
	HoveredPlayerHandle.store(0, std::memory_order_release);
	HoveredCursorX.store(0, std::memory_order_release);
	HoveredCursorY.store(0, std::memory_order_release);
	ItemService = nullptr;
	InventoryService = nullptr;
	ThreadService = nullptr;
	SharedEventService = nullptr;
	PluginContext = nullptr;
}
