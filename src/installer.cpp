#include "installer.h"
#include "install_resource_ids.h"

#include <D2RLPlugin/context.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace QuestRewardReset {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
constexpr auto SharedSetupDirectory = L"plugins/basefiles-and-backups";

auto DisplayPath(fs::path path) -> std::string {
	const auto text = path.make_preferred().u8string();
	return std::string(text.begin(), text.end());
}

class FirstRunInstallLock {
public:
	FirstRunInstallLock() {
		handle_ = CreateMutexW(nullptr, FALSE, L"Local\\D2RLoader.ModFirstRunInstall.v1");
		if (handle_ == nullptr) {
			throw std::runtime_error("could not create the shared first-run install lock");
		}
		const auto result = WaitForSingleObject(handle_, 60000);
		if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
			CloseHandle(handle_);
			handle_ = nullptr;
			throw std::runtime_error("timed out waiting for another plugin's first-run install");
		}
		owns_ = true;
	}

	FirstRunInstallLock(const FirstRunInstallLock&) = delete;
	auto operator=(const FirstRunInstallLock&) -> FirstRunInstallLock& = delete;

	~FirstRunInstallLock() {
		if (owns_) {
			ReleaseMutex(handle_);
		}
		if (handle_ != nullptr) {
			CloseHandle(handle_);
		}
	}

private:
	HANDLE handle_{};
	bool owns_{};
};

struct ResourceFile {
	int resourceId;
	const wchar_t* relativePath;
	std::uint32_t expectedWidth;
	std::uint32_t expectedHeight;
	bool png;
};

constexpr std::array<ResourceFile, 9> InstalledAssets {{
	{ IDR_RESET_AKARA_SPRITE, L"data/hd/global/ui/items/misc/quest/reset_akara.sprite", 98, 98, false },
	{ IDR_RESET_AKARA_LOWEND_SPRITE, L"data/hd/global/ui/items/misc/quest/reset_akara.lowend.sprite", 49, 49, false },
	{ IDR_RESET_CHARSI_SPRITE, L"data/hd/global/ui/items/misc/quest/reset_charsi.sprite", 98, 98, false },
	{ IDR_RESET_CHARSI_LOWEND_SPRITE, L"data/hd/global/ui/items/misc/quest/reset_charsi.lowend.sprite", 49, 49, false },
	{ IDR_RESET_LARZUK_SPRITE, L"data/hd/global/ui/items/misc/quest/reset_larzuk.sprite", 98, 98, false },
	{ IDR_RESET_LARZUK_LOWEND_SPRITE, L"data/hd/global/ui/items/misc/quest/reset_larzuk.lowend.sprite", 49, 49, false },
	{ IDR_RESET_AKARA_PNG, L"data/hd/global/ui/items/misc/quest/reset_akara.png", 98, 98, true },
	{ IDR_RESET_CHARSI_PNG, L"data/hd/global/ui/items/misc/quest/reset_charsi.png", 98, 98, true },
	{ IDR_RESET_LARZUK_PNG, L"data/hd/global/ui/items/misc/quest/reset_larzuk.png", 98, 98, true },
}};

auto ResourceBytes(int id) -> Bytes {
	const auto module = reinterpret_cast<HMODULE>(&__ImageBase);
	const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
	if (resource == nullptr) {
		throw std::runtime_error("embedded installer resource is missing");
	}
	const auto size = SizeofResource(module, resource);
	const auto loaded = LoadResource(module, resource);
	const auto* data = static_cast<const std::uint8_t*>(LockResource(loaded));
	if (size == 0 || data == nullptr) {
		throw std::runtime_error("embedded installer resource is empty");
	}
	return Bytes(data, data + size);
}

auto ReadFile(const fs::path& path) -> Bytes {
	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		throw std::runtime_error("could not read " + DisplayPath(path));
	}
	return Bytes(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

auto BytesToString(const Bytes& bytes) -> std::string {
	if (bytes.empty()) {
		return {};
	}
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

auto StringToBytes(std::string_view value) -> Bytes {
	return Bytes(value.begin(), value.end());
}

auto IsWhitespace(char value) noexcept -> bool {
	return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

auto SplitLines(std::string_view text) -> std::vector<std::string> {
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (start < text.size()) {
		const auto end = text.find('\n', start);
		const auto length = end == std::string_view::npos ? text.size() - start : end - start;
		auto line = std::string(text.substr(start, length));
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (!line.empty()) {
			lines.push_back(std::move(line));
		}
		if (end == std::string_view::npos) {
			break;
		}
		start = end + 1;
	}
	return lines;
}

auto SplitTsv(std::string_view line) -> std::vector<std::string_view> {
	std::vector<std::string_view> fields;
	std::size_t start = 0;
	while (true) {
		const auto end = line.find('\t', start);
		if (end == std::string_view::npos) {
			fields.push_back(line.substr(start));
			break;
		}
		fields.push_back(line.substr(start, end - start));
		start = end + 1;
	}
	return fields;
}

auto NormalizeTsvFields(std::string_view line, std::size_t expectedCount) -> std::vector<std::string> {
	const auto splitFields = SplitTsv(line);
	std::vector<std::string> fields;
	fields.reserve(std::max(splitFields.size(), expectedCount));
	for (const auto field : splitFields) {
		fields.emplace_back(field);
	}
	while (fields.size() > expectedCount && fields.back().empty()) {
		fields.pop_back();
	}
	if (fields.size() > expectedCount) {
		throw std::runtime_error("misc.txt row has non-empty columns beyond the active table schema");
	}
	fields.resize(expectedCount);
	return fields;
}

auto JoinTsv(const std::vector<std::string>& fields) -> std::string {
	std::string line;
	for (std::size_t i = 0; i < fields.size(); ++i) {
		if (i != 0) {
			line.push_back('\t');
		}
		line += fields[i];
	}
	return line;
}

auto MergeMiscRows(std::string_view existing, std::string_view supplied) -> std::string {
	const auto existingLines = SplitLines(existing);
	const auto suppliedLines = SplitLines(supplied);
	if (existingLines.empty() || suppliedLines.size() != 4) {
		throw std::runtime_error("misc.txt or embedded token rows have an unexpected layout");
	}
	auto header = SplitTsv(existingLines.front());
	while (!header.empty() && header.back().empty()) {
		header.pop_back();
	}
	auto sourceHeader = SplitTsv(suppliedLines.front());
	while (!sourceHeader.empty() && sourceHeader.back().empty()) {
		sourceHeader.pop_back();
	}
	if (header.empty() || sourceHeader.empty()) {
		throw std::runtime_error("misc.txt is missing its column header");
	}
	std::map<std::string, std::size_t> targetColumns;
	for (std::size_t i = 0; i < header.size(); ++i) {
		if (header[i].empty() || !targetColumns.emplace(std::string(header[i]), i).second) {
			throw std::runtime_error("misc.txt has an empty or duplicate column name");
		}
	}
	std::set<std::string> sourceColumnNames;
	std::size_t sourceCodeColumn = sourceHeader.size();
	for (std::size_t i = 0; i < sourceHeader.size(); ++i) {
		if (sourceHeader[i].empty() || !sourceColumnNames.insert(std::string(sourceHeader[i])).second) {
			throw std::runtime_error("embedded misc.txt header has an empty or duplicate column name");
		}
		if (sourceHeader[i] == "code") {
			sourceCodeColumn = i;
		}
	}
	if (sourceCodeColumn == sourceHeader.size()) {
		throw std::runtime_error("embedded misc.txt header is missing the code column");
	}
	std::size_t codeColumn = header.size();
	std::size_t spellDescColumn = header.size();
	std::size_t spellDescStrColumn = header.size();
	std::size_t spellDescStr2Column = header.size();
	for (std::size_t i = 0; i < header.size(); ++i) {
		if (header[i] == "code") {
			codeColumn = i;
		} else if (header[i] == "spelldesc") {
			spellDescColumn = i;
		} else if (header[i] == "spelldescstr") {
			spellDescStrColumn = i;
		} else if (header[i] == "spelldescstr2") {
			spellDescStr2Column = i;
		}
	}
	if (codeColumn == header.size()) {
		throw std::runtime_error("misc.txt is missing a code column");
	}

	std::set<std::string> expectedCodes;
	std::map<std::string, std::string> rowsByCode;
	for (std::size_t lineIndex = 1; lineIndex < suppliedLines.size(); ++lineIndex) {
		const auto sourceFields = NormalizeTsvFields(suppliedLines[lineIndex], sourceHeader.size());
		const auto& code = sourceFields[sourceCodeColumn];
		if (code != "rt1" && code != "rt2" && code != "rt3") {
			throw std::runtime_error("embedded misc.txt token row has an unexpected item code");
		}
		if (!expectedCodes.insert(code).second) {
			throw std::runtime_error("embedded misc.txt token rows repeat an item code");
		}
		std::vector<std::string> fields(header.size());
		for (std::size_t sourceColumn = 0; sourceColumn < sourceHeader.size(); ++sourceColumn) {
			const auto targetColumn = targetColumns.find(std::string(sourceHeader[sourceColumn]));
			if (targetColumn != targetColumns.end()) {
				fields[targetColumn->second] = sourceFields[sourceColumn];
			}
		}
		if (codeColumn >= fields.size() || fields[codeColumn] != code) {
			throw std::runtime_error("embedded misc.txt token row could not be mapped to the active table schema");
		}
		rowsByCode.emplace(code, JoinTsv(fields));
	}
	if (expectedCodes.size() != 3) {
		throw std::runtime_error("embedded misc.txt rows do not contain all three reset tokens");
	}

	std::set<std::string> foundCodes;
	std::vector<std::pair<std::string, std::string>> replacements;
	for (std::size_t i = 1; i < existingLines.size(); ++i) {
		const auto fields = SplitTsv(existingLines[i]);
		if (fields.size() <= codeColumn || rowsByCode.find(std::string(fields[codeColumn])) == rowsByCode.end()) {
			continue;
		}
		const std::string code(fields[codeColumn]);
		if (!foundCodes.insert(code).second) {
			throw std::runtime_error("misc.txt already has a conflicting reset-token row");
		}
		const auto expectedLine = rowsByCode.at(code);
		if (existingLines[i] == expectedLine) {
			continue;
		}
		const auto normalizedFields = NormalizeTsvFields(existingLines[i], header.size());
		const auto expectedFields = SplitTsv(expectedLine);
		bool matchesExceptTooltip = normalizedFields.size() == expectedFields.size();
		for (std::size_t field = 0; matchesExceptTooltip && field < normalizedFields.size(); ++field) {
			if (field != spellDescColumn && field != spellDescStrColumn && field != spellDescStr2Column
				&& normalizedFields[field] != expectedFields[field]) {
				matchesExceptTooltip = false;
			}
		}
		if (!matchesExceptTooltip) {
			throw std::runtime_error("misc.txt already has a conflicting reset-token row");
		}
		replacements.emplace_back(existingLines[i], expectedLine);
	}

	std::vector<std::string> missing;
	for (const auto& [code, row] : rowsByCode) {
		if (foundCodes.find(code) == foundCodes.end()) {
			missing.push_back(row);
		}
	}
	if (missing.empty() && replacements.empty()) {
		return std::string(existing);
	}

	const std::string newline = existing.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
	std::string result(existing);
	for (const auto& [oldLine, newLine] : replacements) {
		const auto position = result.find(oldLine);
		if (position == std::string::npos) {
			throw std::runtime_error("misc.txt reset-token tooltip row could not be updated safely");
		}
		result.replace(position, oldLine.size(), newLine);
	}
	if (result.empty() || result.back() != '\n') {
		result += newline;
	}
	for (std::size_t i = 0; i < missing.size(); ++i) {
		result += missing[i];
		result += newline;
	}
	return result;
}

auto ExtractJsonObjects(std::string_view source) -> std::vector<std::string> {
	std::vector<std::string> objects;
	std::size_t start = std::string_view::npos;
	std::uint32_t depth = 0;
	bool inString = false;
	bool escaped = false;
	for (std::size_t i = 0; i < source.size(); ++i) {
		const char character = source[i];
		if (inString) {
			if (escaped) {
				escaped = false;
			} else if (character == '\\') {
				escaped = true;
			} else if (character == '"') {
				inString = false;
			}
			continue;
		}
		if (character == '"') {
			inString = true;
		} else if (character == '{') {
			if (depth == 0) {
				start = i;
			}
			++depth;
		} else if (character == '}') {
			if (depth == 0) {
				throw std::runtime_error("embedded item-name JSON has unmatched braces");
			}
			--depth;
			if (depth == 0) {
				objects.emplace_back(source.substr(start, i - start + 1));
			}
		}
	}
	if (depth != 0 || inString || objects.size() != 3) {
		throw std::runtime_error("embedded item-name JSON does not contain three complete objects");
	}
	return objects;
}

auto JsonStringProperty(const std::string& object, const std::regex& expression) -> std::string {
	std::smatch match;
	if (!std::regex_search(object, match, expression) || match.size() < 2) {
		throw std::runtime_error("embedded item-name JSON has a missing property");
	}
	return match[1].str();
}

const std::regex StringIdExpression(R"("id"\s*:\s*(\d+))");

void CollectStringIds(const std::string& text, std::set<std::uint32_t>& usedIds) {
	for (auto it = std::sregex_iterator(text.begin(), text.end(), StringIdExpression);
		it != std::sregex_iterator(); ++it) {
		const auto value = (*it)[1].str();
		std::uint32_t id {};
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
		if (parsed.ec == std::errc{} && id <= 65535) {
			usedIds.insert(id);
		}
	}
}

auto AppendJsonEntries(std::string_view existing, std::size_t close,
	const std::vector<std::string>& additions) -> std::string {
	if (additions.empty()) {
		return std::string(existing);
	}
	const auto end = existing.find_last_not_of(" \t\r\n", close - 1);
	if (end == std::string_view::npos
		|| (existing[end] != '[' && existing[end] != '}' && existing[end] != ',')) {
		throw std::runtime_error("unsupported JSON array ending");
	}
	const auto newline = existing.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";
	std::string result(existing.substr(0, end + 1));
	for (const auto& entry : additions) {
		if (result.back() == '}') {
			result += ',';
		}
		result += newline;
		result += "  ";
		result += entry;
	}
	result += newline;
	result.append(existing.substr(close));
	return result;
}

auto MergeItemNames(std::string_view existing, std::string_view supplied,
	std::set<std::uint32_t>& usedIds) -> std::string {
	const auto first = existing.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
	const auto close = existing.find_last_not_of(" \t\r\n");
	if (first == std::string_view::npos || close == std::string_view::npos
		|| existing[first] != '[' || existing[close] != ']') {
		throw std::runtime_error("item-names.json is not a top-level JSON array");
	}

	const std::regex keyExpression(R"REGEX("Key"\s*:\s*"([^"]+)")REGEX");
	std::set<std::string> existingKeys;
	const std::string existingCopy(existing);
	for (auto it = std::sregex_iterator(existingCopy.begin(), existingCopy.end(), keyExpression);
		it != std::sregex_iterator(); ++it) {
		existingKeys.insert((*it)[1].str());
	}

	auto templates = ExtractJsonObjects(supplied);
	std::map<std::string, std::string> objectsByKey;
	for (auto& object : templates) {
		const auto key = JsonStringProperty(object, keyExpression);
		if (key != "rt1" && key != "rt2" && key != "rt3") {
			throw std::runtime_error("embedded item-name JSON contains an unexpected key");
		}
		if (!objectsByKey.emplace(key, object).second) {
			throw std::runtime_error("embedded item-name JSON repeats a key");
		}
	}
	if (objectsByKey.size() != 3) {
		throw std::runtime_error("embedded item-name JSON is missing a token key");
	}

	std::vector<std::string> additions;
	std::uint32_t nextId = 63000;
	for (const auto& key : { "rt1", "rt2", "rt3" }) {
		if (existingKeys.find(key) != existingKeys.end()) {
			continue;
		}
		while (nextId < 65000 && !usedIds.insert(nextId).second) {
			++nextId;
		}
		if (nextId == 65000) {
			throw std::runtime_error("no unused localization ID in the range 63000 to 64999");
		}

		auto object = objectsByKey.at(key);
		std::smatch idMatch;
		if (!std::regex_search(object, idMatch, StringIdExpression)) {
			throw std::runtime_error("embedded item-name JSON object is missing its ID");
		}
		object.replace(static_cast<std::size_t>(idMatch.position()), static_cast<std::size_t>(idMatch.length()),
			"\"id\": " + std::to_string(nextId++));
		additions.push_back(std::move(object));
	}
	return AppendJsonEntries(existing, close, additions);
}

auto MergeItemAssetMappings(std::string_view existing, std::string_view supplied) -> std::string {
	const auto first = existing.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
	const auto close = existing.find_last_not_of(" \t\r\n");
	if (first == std::string_view::npos || close == std::string_view::npos
		|| existing[first] != '[' || existing[close] != ']') {
		throw std::runtime_error("items.json is not a top-level JSON array");
	}

	const std::regex mappingExpression(
		R"REGEX(\{\s*"([^"]+)"\s*:\s*\{\s*"asset"\s*:\s*"([^"]+)"\s*\}\s*\})REGEX");
	const std::array<std::pair<std::string_view, std::string_view>, 3> expected {{
		{ "rt1", "quest/reset_akara" },
		{ "rt2", "quest/reset_charsi" },
		{ "rt3", "quest/reset_larzuk" },
	}};
	std::map<std::string, std::pair<std::string, std::string>> mappings;
	for (const auto& object : ExtractJsonObjects(supplied)) {
		std::smatch match;
		if (!std::regex_search(object, match, mappingExpression) || match.size() < 3) {
			throw std::runtime_error("embedded item mapping has an unexpected layout");
		}
		const auto key = match[1].str();
		const auto asset = match[2].str();
		const auto expectedMapping = std::find_if(expected.begin(), expected.end(), [&](const auto& item) {
			return item.first == key && item.second == asset;
		});
		if (expectedMapping == expected.end() || !mappings.emplace(key, std::make_pair(asset, object)).second) {
			throw std::runtime_error("embedded item mapping contains an unexpected or duplicate entry");
		}
	}
	if (mappings.size() != expected.size()) {
		throw std::runtime_error("embedded item mappings are missing a token code");
	}

	const std::string existingCopy(existing);
	std::set<std::string> found;
	for (auto it = std::sregex_iterator(existingCopy.begin(), existingCopy.end(), mappingExpression);
		it != std::sregex_iterator(); ++it) {
		const auto key = (*it)[1].str();
		const auto expectedItem = mappings.find(key);
		if (expectedItem == mappings.end()) {
			continue;
		}
		if ((*it)[2].str() != expectedItem->second.first || !found.insert(key).second) {
			throw std::runtime_error("items.json already has a conflicting reset-token sprite mapping");
		}
	}
	for (const auto& [key, mapping] : mappings) {
		(void)mapping;
		const std::regex keyExpression("\\\"" + key + "\\\"\\s*:");
		if (std::regex_search(existingCopy, keyExpression) && found.find(key) == found.end()) {
			throw std::runtime_error("items.json has an unsupported or conflicting reset-token mapping");
		}
	}

	std::vector<std::string> additions;
	for (const auto& [key, mapping] : mappings) {
		if (found.find(key) == found.end()) {
			additions.push_back(mapping.second);
		}
	}
	return AppendJsonEntries(existing, close, additions);
}

auto ReadLe16(const Bytes& bytes, std::size_t offset) -> std::uint16_t {
	return static_cast<std::uint16_t>(bytes[offset])
		| static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

auto ReadLe32(const Bytes& bytes, std::size_t offset) -> std::uint32_t {
	return static_cast<std::uint32_t>(bytes[offset])
	| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
	| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
	| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void ValidateAsset(const ResourceFile& asset, const Bytes& data) {
	if (asset.png) {
		constexpr std::array<std::uint8_t, 8> signature { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		if (data.size() < 24 || !std::equal(signature.begin(), signature.end(), data.begin())
			|| data[12] != 'I' || data[13] != 'H' || data[14] != 'D' || data[15] != 'R') {
			throw std::runtime_error("embedded PNG has an invalid header");
		}
		const auto width = (static_cast<std::uint32_t>(data[16]) << 24)
			| (static_cast<std::uint32_t>(data[17]) << 16)
			| (static_cast<std::uint32_t>(data[18]) << 8)
			| static_cast<std::uint32_t>(data[19]);
		const auto height = (static_cast<std::uint32_t>(data[20]) << 24)
			| (static_cast<std::uint32_t>(data[21]) << 16)
			| (static_cast<std::uint32_t>(data[22]) << 8)
			| static_cast<std::uint32_t>(data[23]);
		if (width != asset.expectedWidth || height != asset.expectedHeight) {
			throw std::runtime_error("embedded PNG dimensions are unexpected");
		}
		return;
	}
	if (data.size() < 40 || std::string_view(reinterpret_cast<const char*>(data.data()), 4) != "SpA1"
		|| ReadLe16(data, 6) != asset.expectedWidth || ReadLe32(data, 12) != asset.expectedHeight
		|| data.size() != 40ull + static_cast<std::uint64_t>(asset.expectedWidth) * asset.expectedHeight * 4ull) {
		throw std::runtime_error("embedded SpA1 sprite dimensions or byte count are unexpected");
	}
}

auto IsWithin(const fs::path& root, const fs::path& target) -> bool {
	const auto relative = target.lexically_relative(root);
	return !relative.empty() && !relative.has_root_path()
		&& *relative.begin() != L"." && *relative.begin() != L"..";
}

auto CheckedPath(const fs::path& root, const fs::path& relative) -> fs::path {
	const auto target = root / relative;
	if (!IsWithin(root, fs::weakly_canonical(target))) {
		throw std::runtime_error("installation path is outside its expected folder: " + DisplayPath(target));
	}
	return target;
}

auto FindActiveModRoot(const D2RL::PluginContext& context) -> fs::path {
	if (context.activeMod == nullptr || context.activeMod[0] == '\0') {
		return {};
	}
	const std::string_view name(context.activeMod);
	const fs::path activeMod(std::u8string(name.begin(), name.end()));
	if (activeMod.has_parent_path() || activeMod.has_root_path()
		|| activeMod == L"." || activeMod == L".." || activeMod.filename() != activeMod) {
		return {};
	}
	std::vector<fs::path> candidates;
	const fs::path modDirectory = context.modDirectory == nullptr ? fs::path{} : fs::path(context.modDirectory);
	const fs::path scopeRoot = context.scopeRootDirectory == nullptr ? fs::path{} : fs::path(context.scopeRootDirectory);
	const fs::path packageName = activeMod.native() + L".mpq";
	const auto addBase = [&](const fs::path& base) {
		if (base.empty()) {
			return;
		}
		candidates.push_back(base / activeMod / packageName);
		candidates.push_back(base / packageName);
		candidates.push_back(base / activeMod);
		candidates.push_back(base);
	};
	addBase(modDirectory);
	if (!scopeRoot.empty()) {
		candidates.push_back(scopeRoot / activeMod / packageName);
		candidates.push_back(scopeRoot / packageName);
		candidates.push_back(scopeRoot / activeMod);
		candidates.push_back(scopeRoot / L"mods" / activeMod / packageName);
		addBase(scopeRoot);
	}
	fs::path expected;
	std::set<std::wstring> seen;
	for (const auto& candidate : candidates) {
		const auto normalized = candidate.lexically_normal();
		if (!seen.insert(normalized.native()).second) {
			continue;
		}
		if (normalized.filename() == packageName) {
			if (fs::exists(normalized)) {
				return fs::canonical(normalized);
			}
			if (expected.empty() && fs::is_directory(normalized.parent_path())) {
				expected = normalized;
			}
		} else if (normalized.filename() == activeMod && fs::is_directory(normalized / L"data")) {
			return fs::canonical(normalized);
		}
	}
	return expected.empty() ? fs::path{} : fs::weakly_canonical(fs::absolute(expected));
}

auto ReadTarget(const fs::path& root, const fs::path& relative) -> std::optional<Bytes> {
	const auto target = CheckedPath(root, relative);
	std::error_code error;
	const auto status = fs::symlink_status(target, error);
	if (status.type() == fs::file_type::not_found) {
		return std::nullopt;
	}
	if (error || !fs::is_regular_file(status)) {
		throw std::runtime_error("expected a readable, loose file here: " + DisplayPath(target));
	}
	return ReadFile(target);
}

auto ReadRequiredFile(const fs::path& root, const fs::path& relative) -> Bytes {
	auto bytes = ReadTarget(root, relative);
	if (!bytes) {
		throw std::runtime_error("required loose file disappeared during setup: " + DisplayPath(root / relative));
	}
	return std::move(*bytes);
}

struct InstallInput {
	std::optional<Bytes> loose;
	Bytes base;

	auto Source() const -> const Bytes& { return loose ? *loose : base; }
};

auto ReadInstallInput(const D2RL::PluginContext& context, const fs::path& modRoot,
	const fs::path& loaderRoot, const fs::path& relative) -> std::optional<InstallInput> {
	if (auto loose = ReadTarget(modRoot, relative)) {
		return InstallInput{ std::move(loose), {} };
	}
	const auto baseRoot = fs::weakly_canonical(CheckedPath(loaderRoot,
		fs::path(SharedSetupDirectory) / L"basefiles"));
	const auto baseRelative = relative.lexically_relative(L"data");
	if (auto base = ReadTarget(baseRoot, baseRelative)) {
		return InstallInput{ std::nullopt, std::move(*base) };
	}
	const auto message = "Quest Reward Reset: missing required file " + DisplayPath(modRoot / relative)
		+ ". Put an extracted copy here: " + DisplayPath(baseRoot / baseRelative);
	context.LogError(message.c_str());
	return std::nullopt;
}

auto Timestamp() -> std::wstring {
	const auto now = std::chrono::system_clock::now();
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
	const auto time = std::chrono::system_clock::to_time_t(now);
	tm localTime {};
	localtime_s(&localTime, &time);
	wchar_t buffer[48] {};
	swprintf_s(buffer, std::size(buffer), L"%04d%02d%02d-%02d%02d%02d-%03lld-%lu",
		localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
		localTime.tm_hour, localTime.tm_min, localTime.tm_sec,
		static_cast<long long>(milliseconds), GetCurrentProcessId());
	return buffer;
}

void WriteAtomic(const fs::path& target, const Bytes& data, bool replace) {
	fs::create_directories(target.parent_path());
	auto temporary = target;
	temporary += L".qrr-install-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
	try {
		std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
		if (!data.empty()) {
			stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
		}
		stream.close();
		const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
		if (!stream || !MoveFileExW(temporary.c_str(), target.c_str(), flags)) {
			throw std::runtime_error("could not write " + DisplayPath(target));
		}
	} catch (...) {
		std::error_code ignored;
		fs::remove(temporary, ignored);
		throw;
	}
}

struct Edit {
	fs::path relative;
	std::optional<Bytes> before;
	Bytes after;
};

void QueueEdit(std::vector<Edit>& edits, fs::path relative, std::optional<Bytes> before, Bytes after) {
	if (!before || *before != after) {
		edits.push_back({ std::move(relative), std::move(before), std::move(after) });
	}
}

void ApplyEdits(const fs::path& modRoot, const fs::path& loaderRoot, const fs::path& backupRoot,
	const std::vector<Edit>& edits, const fs::path& markerRelative, const Bytes& marker) {
	if (!edits.empty()) {
		if (!fs::create_directories(backupRoot)) {
			throw std::runtime_error("install backup directory already exists: " + DisplayPath(backupRoot));
		}
		std::string created;
		for (const auto& edit : edits) {
			if (!edit.before) {
				created += DisplayPath(edit.relative) + '\n';
				continue;
			}
			const auto backup = CheckedPath(backupRoot, edit.relative);
			fs::create_directories(backup.parent_path());
			if (!fs::copy_file(CheckedPath(modRoot, edit.relative), backup) || ReadFile(backup) != *edit.before) {
				throw std::runtime_error("could not verify backup of " + DisplayPath(modRoot / edit.relative));
			}
		}
		if (!created.empty()) {
			WriteAtomic(backupRoot / L"created-files.txt", StringToBytes(created), false);
		}
	}

	std::vector<const Edit*> written;
	written.reserve(edits.size());
	try {
		for (const auto& edit : edits) {
			const auto target = CheckedPath(modRoot, edit.relative);
			if (ReadTarget(modRoot, edit.relative) != edit.before) {
				throw std::runtime_error("a mod file changed during setup: " + DisplayPath(target));
			}
			WriteAtomic(target, edit.after, edit.before.has_value());
			written.push_back(&edit);
		}
		// Commit the marker last so a failed or interrupted install can be retried.
		WriteAtomic(CheckedPath(loaderRoot, markerRelative), marker, false);
	} catch (...) {
		bool restored = true;
		for (auto i = written.rbegin(); i != written.rend(); ++i) {
			try {
				const auto& edit = **i;
				const auto current = ReadTarget(modRoot, edit.relative);
				if (!current || *current != edit.after) {
					restored = false;
					continue;
				}
				const auto target = CheckedPath(modRoot, edit.relative);
				if (edit.before) {
					WriteAtomic(target, *edit.before, true);
				} else if (!fs::remove(target)) {
					restored = false;
				}
			} catch (...) {
				restored = false;
			}
		}
		if (!restored) {
			throw std::runtime_error("could not restore every edited file; see the install backup at "
				+ DisplayPath(backupRoot));
		}
		throw;
	}
}

auto Install(const D2RL::PluginContext& context, std::string_view pluginVersion) -> InstallResult {
	if (context.pluginConfigPath == nullptr || context.pluginConfigPath[0] == L'\0') {
		throw std::runtime_error("D2RLoader did not provide the mod's configuration path");
	}
	const auto configDirectory = fs::path(context.pluginConfigPath).parent_path();
	const auto loader = configDirectory.parent_path();
	if (!loader.is_absolute() || loader.filename() != L"d2rloader" || configDirectory.filename() != L"config") {
		throw std::runtime_error("D2RLoader did not provide a mod-scoped configuration path");
	}

	const auto modRoot = FindActiveModRoot(context);
	if (modRoot.empty()) {
		throw std::runtime_error("could not resolve the active mod's data directory");
	}
	const auto mod = modRoot.extension() == L".mpq" ? modRoot.parent_path() : modRoot;
	const auto loaderRoot = fs::weakly_canonical(loader);
	if (!IsWithin(mod, loaderRoot) || loaderRoot.filename() != L"d2rloader") {
		throw std::runtime_error("D2RLoader's configuration and backup folder is outside the active mod");
	}

	FirstRunInstallLock installLock;
	const fs::path markerRelative(L"config/quest-reward-reset.toml");
	const auto markerPath = CheckedPath(loaderRoot, markerRelative);
	std::error_code error;
	const auto markerStatus = fs::symlink_status(markerPath, error);
	if (fs::is_regular_file(markerStatus)) {
		return InstallResult::AlreadyInstalled;
	}
	if (markerStatus.type() != fs::file_type::not_found) {
		throw std::runtime_error("expected a regular file at " + DisplayPath(markerPath));
	}

	if (fs::is_regular_file(modRoot)) {
		throw std::runtime_error("first-run setup requires an unpacked mod directory: " + DisplayPath(modRoot));
	}
	const fs::path miscRelative(L"data/global/excel/misc.txt");
	const fs::path stringsRelative(L"data/local/lng/strings/item-names.json");
	const fs::path mappingsRelative(L"data/hd/items/items.json");
	auto misc = ReadInstallInput(context, modRoot, loaderRoot, miscRelative);
	auto names = ReadInstallInput(context, modRoot, loaderRoot, stringsRelative);
	auto mappings = ReadInstallInput(context, modRoot, loaderRoot, mappingsRelative);
	if (!misc || !names || !mappings) {
		return InstallResult::Failed;
	}

	std::vector<Edit> edits;
	edits.reserve(3 + InstalledAssets.size());
	auto mergedMisc = MergeMiscRows(BytesToString(misc->Source()), BytesToString(ResourceBytes(IDR_RESET_TOKEN_MISC_ROWS)));
	QueueEdit(edits, miscRelative, std::move(misc->loose), StringToBytes(mergedMisc));

	const auto namesText = BytesToString(names->Source());
	std::set<std::uint32_t> usedIds;
	CollectStringIds(namesText, usedIds);
	const auto stringsDir = CheckedPath(modRoot, stringsRelative.parent_path());
	const auto stringsStatus = fs::status(stringsDir, error);
	if (fs::is_directory(stringsStatus)) {
		for (const auto& file : fs::directory_iterator(stringsDir)) {
			if (file.path().extension() != L".json" || file.path().filename() == stringsRelative.filename()
				|| !file.is_regular_file()) {
				continue;
			}
			const auto relative = stringsRelative.parent_path() / file.path().filename();
			CollectStringIds(BytesToString(ReadRequiredFile(modRoot, relative)), usedIds);
		}
	} else if (stringsStatus.type() != fs::file_type::not_found) {
		throw std::runtime_error("expected a readable directory here: " + DisplayPath(stringsDir));
	}
	auto mergedNames = MergeItemNames(namesText, BytesToString(ResourceBytes(IDR_RESET_TOKEN_ITEM_NAMES)), usedIds);
	QueueEdit(edits, stringsRelative, std::move(names->loose), StringToBytes(mergedNames));

	auto mergedMappings = MergeItemAssetMappings(
		BytesToString(mappings->Source()), BytesToString(ResourceBytes(IDR_RESET_TOKEN_ITEM_MAPPINGS)));
	QueueEdit(edits, mappingsRelative, std::move(mappings->loose), StringToBytes(mergedMappings));

	for (const auto& asset : InstalledAssets) {
		auto bytes = ResourceBytes(asset.resourceId);
		ValidateAsset(asset, bytes);
		QueueEdit(edits, asset.relativePath, ReadTarget(modRoot, asset.relativePath), std::move(bytes));
	}

	const auto backupRoot = CheckedPath(loaderRoot,
		fs::path(SharedSetupDirectory) / L"backups/quest-reward-reset" / Timestamp());
	const auto marker = std::string("[installation]\ncomplete = true\nplugin_version = \"")
		+ std::string(pluginVersion) + "\"\n";
	ApplyEdits(modRoot, loaderRoot, backupRoot, edits, markerRelative, StringToBytes(marker));
	if (edits.empty()) {
		context.LogInfo("Quest Reward Reset: token data already present; recorded first-run installation.");
		return InstallResult::AlreadyInstalled;
	}
	context.LogInfo("Quest Reward Reset: first-run install complete. Exit and restart D2RLoader with the same mod to load the tokens.");
	return InstallResult::Installed;
}

} // namespace

auto RunFirstRunInstall(const D2RL::PluginContext& context, std::string_view pluginVersion) noexcept -> InstallResult {
	try {
		return Install(context, pluginVersion);
	} catch (const std::exception& error) {
		char message[2048] {};
		std::snprintf(message, sizeof(message), "Quest Reward Reset: first-run setup failed. %s", error.what());
		context.LogError(message);
	} catch (...) {
		context.LogError("Quest Reward Reset: first-run setup failed unexpectedly.");
	}
	return InstallResult::Failed;
}

} // namespace QuestRewardReset
