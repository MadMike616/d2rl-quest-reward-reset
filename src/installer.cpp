#include "installer.h"
#include "install_resource_ids.h"

#include <D2RLPlugin/context.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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
		throw std::runtime_error("could not open an installation target");
	}
	return Bytes(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

auto BytesToString(const Bytes& bytes) -> std::string {
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

auto StringToBytes(std::string_view value) -> Bytes {
	return Bytes(value.begin(), value.end());
}

auto ReadText(const fs::path& path) -> std::string {
	return BytesToString(ReadFile(path));
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

auto MergeItemNames(std::string_view existing, std::string_view supplied) -> std::string {
	const auto first = existing.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
	const auto close = existing.find_last_not_of(" \t\r\n");
	if (first == std::string_view::npos || close == std::string_view::npos
		|| existing[first] != '[' || existing[close] != ']') {
		throw std::runtime_error("item-names.json is not a top-level JSON array");
	}

	const std::regex keyExpression(R"REGEX("Key"\s*:\s*"([^"]+)")REGEX");
	const std::regex idExpression(R"("id"\s*:\s*(\d+))");
	std::set<std::string> existingKeys;
	std::set<std::uint32_t> usedIds;
	const std::string existingCopy(existing);
	for (auto it = std::sregex_iterator(existingCopy.begin(), existingCopy.end(), keyExpression);
		it != std::sregex_iterator(); ++it) {
		existingKeys.insert((*it)[1].str());
	}
	for (auto it = std::sregex_iterator(existingCopy.begin(), existingCopy.end(), idExpression);
		it != std::sregex_iterator(); ++it) {
		const auto parsed = std::stoul((*it)[1].str());
		if (parsed <= 65535) {
			usedIds.insert(static_cast<std::uint32_t>(parsed));
		}
	}

	auto templates = ExtractJsonObjects(supplied);
	std::map<std::string, std::string> objectsByKey;
	std::uint32_t maximumId = usedIds.empty() ? 0 : *usedIds.rbegin();
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
	std::uint32_t nextId = maximumId < 65535 ? maximumId + 1 : 1;
	for (const auto& key : { "rt1", "rt2", "rt3" }) {
		if (existingKeys.find(key) != existingKeys.end()) {
			continue;
		}
		std::uint32_t candidate = nextId;
		bool found = false;
		for (std::uint32_t attempt = 0; attempt < 65535; ++attempt) {
			if (candidate == 0 || candidate > 65535) {
				candidate = 1;
			}
			if (usedIds.find(candidate) == usedIds.end()) {
				found = true;
				break;
			}
			++candidate;
		}
		if (!found) {
			throw std::runtime_error("item-names.json has no unused string ID in the supported range");
		}
		usedIds.insert(candidate);
		nextId = candidate + 1;

		auto object = objectsByKey.at(key);
		std::smatch idMatch;
		if (!std::regex_search(object, idMatch, idExpression)) {
			throw std::runtime_error("embedded item-name JSON object is missing its ID");
		}
		object.replace(static_cast<std::size_t>(idMatch.position()), static_cast<std::size_t>(idMatch.length()),
			"\"id\": " + std::to_string(candidate));
		additions.push_back(std::move(object));
	}
	if (additions.empty()) {
		return std::string(existing);
	}

	const auto arrayOpen = existing.find('[', first);
	const auto innerStart = arrayOpen + 1;
	std::size_t innerEnd = close;
	while (innerEnd > innerStart && IsWhitespace(existing[innerEnd - 1])) {
		--innerEnd;
	}
	const bool hasExistingEntries = existing.find_first_not_of(" \t\r\n", innerStart) < close;
	std::string result(existing.substr(0, innerEnd));
	if (hasExistingEntries) {
		result += ',';
	}
	result += "\n  ";
	for (std::size_t i = 0; i < additions.size(); ++i) {
		if (i != 0) {
			result += ",\n  ";
		}
		result += additions[i];
	}
	result += '\n';
	result.append(existing.substr(close));
	return result;
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
	if (additions.empty()) {
		return std::string(existing);
	}

	const auto arrayOpen = existing.find('[', first);
	const auto innerStart = arrayOpen + 1;
	std::size_t innerEnd = close;
	while (innerEnd > innerStart && IsWhitespace(existing[innerEnd - 1])) {
		--innerEnd;
	}
	const bool hasExistingEntries = existing.find_first_not_of(" \t\r\n", innerStart) < close;
	std::string result(existing.substr(0, innerEnd));
	if (hasExistingEntries) {
		result += ',';
	}
	result += "\n  ";
	for (std::size_t i = 0; i < additions.size(); ++i) {
		if (i != 0) {
			result += ",\n  ";
		}
		result += additions[i];
	}
	result += '\n';
	result.append(existing.substr(close));
	return result;
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

auto FindActiveModRoot(const D2RL::PluginContext* context) -> fs::path {
	if (context == nullptr || context->activeMod == nullptr || context->activeMod[0] == '\0') {
		return {};
	}
	const fs::path activeMod(context->activeMod);
	if (activeMod.has_parent_path() || activeMod.has_root_path()
		|| activeMod == L"." || activeMod == L".." || activeMod.filename() != activeMod) {
		return {};
	}
	std::vector<fs::path> candidates;
	const fs::path modDirectory = context->modDirectory == nullptr ? fs::path{} : fs::path(context->modDirectory);
	const fs::path scopeRoot = context->scopeRootDirectory == nullptr ? fs::path{} : fs::path(context->scopeRootDirectory);
	const auto packageName = activeMod.native() + std::wstring(L".mpq");
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
	std::set<std::wstring> seen;
	for (const auto& candidate : candidates) {
		const auto normalized = candidate.lexically_normal();
		if (!seen.insert(normalized.native()).second) {
			continue;
		}
		const auto folderName = normalized.filename();
		if (folderName != activeMod && folderName != fs::path(packageName)) {
			continue;
		}
		std::error_code error;
		if (fs::is_regular_file(normalized / L"data/global/excel/misc.txt", error) && !error
			&& fs::is_regular_file(normalized / L"data/local/lng/strings/item-names.json", error) && !error) {
			return normalized;
		}
	}
	return {};
}

auto Timestamp() -> std::wstring {
	const auto now = std::chrono::system_clock::now();
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
	const auto time = std::chrono::system_clock::to_time_t(now);
	tm localTime {};
	localtime_s(&localTime, &time);
	wchar_t buffer[40] {};
	swprintf_s(buffer, std::size(buffer), L"%04d%02d%02d-%02d%02d%02d-%03lld",
		localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
		localTime.tm_hour, localTime.tm_min, localTime.tm_sec,
		static_cast<long long>(milliseconds));
	return buffer;
}

void WriteAtomic(const fs::path& target, const Bytes& data) {
	std::error_code error;
	fs::create_directories(target.parent_path(), error);
	if (error) {
		throw std::runtime_error("could not create an installation directory");
	}
	const auto temporary = target.wstring() + L".qrr-install-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
	{
		std::ofstream stream(fs::path(temporary), std::ios::binary | std::ios::trunc);
		if (!stream) {
			throw std::runtime_error("could not create a temporary installation file");
		}
		stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
		stream.flush();
		if (!stream) {
			stream.close();
			fs::remove(temporary, error);
			throw std::runtime_error("could not write a temporary installation file");
		}
	}
	if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		fs::remove(temporary, error);
		throw std::runtime_error("could not atomically replace an installation file");
	}
}

void Install(const D2RL::PluginContext* context) {
	if (context->pluginConfigPath == nullptr || context->pluginConfigPath[0] == L'\0') {
		context->LogError("Quest Reward Reset: D2RLoader did not provide the plugin TOML path; first-run install was skipped.");
		return;
	}
	const fs::path loaderConfigPath(context->pluginConfigPath);
	const fs::path configDirectory = loaderConfigPath.parent_path();
	if (configDirectory.empty()) {
		context->LogError("Quest Reward Reset: D2RLoader's plugin config directory could not be resolved; first-run install was skipped.");
		return;
	}
	const fs::path configPath = configDirectory / L"quest-reward-reset.toml";
	std::error_code error;
	const bool configExists = fs::exists(configPath, error);
	if (error) {
		context->LogError("Quest Reward Reset: could not inspect the plugin TOML path; first-run install was skipped.");
		return;
	}
	if (configExists) {
		if (fs::is_regular_file(configPath, error) && !error) {
			context->LogInfo("Quest Reward Reset: plugin TOML already exists; skipping mod-file installation.");
		} else {
			context->LogError("Quest Reward Reset: plugin TOML path exists but is not a regular file; first-run install was skipped.");
		}
		return;
	}

	const auto modRoot = FindActiveModRoot(context);
	if (modRoot.empty()) {
		context->LogError("Quest Reward Reset: could not resolve the active mod data folder; first-run install was skipped.");
		return;
	}
	FirstRunInstallLock installLock;
	const bool configExistsAfterLock = fs::exists(configPath, error);
	if (error) {
		context->LogError("Quest Reward Reset: could not inspect the plugin TOML path after waiting for the install lock; first-run install was skipped.");
		return;
	}
	if (configExistsAfterLock) {
		if (fs::is_regular_file(configPath, error) && !error) {
			context->LogInfo("Quest Reward Reset: another plugin instance completed installation; skipping mod-file edits.");
		} else {
			context->LogError("Quest Reward Reset: plugin TOML path exists but is not a regular file; first-run install was skipped.");
		}
		return;
	}
	const auto miscRelative = fs::path(L"data/global/excel/misc.txt");
	const auto stringsRelative = fs::path(L"data/local/lng/strings/item-names.json");
	std::map<fs::path, Bytes> desired;
	const auto mergedMisc = MergeMiscRows(
		ReadText(modRoot / miscRelative), BytesToString(ResourceBytes(IDR_RESET_TOKEN_MISC_ROWS)));
	const auto mergedNames = MergeItemNames(
		ReadText(modRoot / stringsRelative), BytesToString(ResourceBytes(IDR_RESET_TOKEN_ITEM_NAMES)));
	const auto itemMappingsRelative = fs::path(L"data/hd/items/items.json");
	const auto mergedItemMappings = MergeItemAssetMappings(
		ReadText(modRoot / itemMappingsRelative), BytesToString(ResourceBytes(IDR_RESET_TOKEN_ITEM_MAPPINGS)));
	desired.emplace(miscRelative, StringToBytes(mergedMisc));
	desired.emplace(stringsRelative, StringToBytes(mergedNames));
	desired.emplace(itemMappingsRelative, StringToBytes(mergedItemMappings));
	for (const auto& asset : InstalledAssets) {
		auto bytes = ResourceBytes(asset.resourceId);
		ValidateAsset(asset, bytes);
		desired.emplace(fs::path(asset.relativePath), std::move(bytes));
	}

	if (context->scopeRootDirectory == nullptr || context->scopeRootDirectory[0] == L'\0') {
		context->LogError("Quest Reward Reset: D2RLoader did not provide the mod scope folder; files were not changed.");
		return;
	}
	const fs::path loaderDirectory = configPath.parent_path().parent_path();
	if (loaderDirectory.empty()) {
		context->LogError("Quest Reward Reset: could not resolve the mod's D2RLoader folder; files were not changed.");
		return;
	}
	const fs::path backupRoot = loaderDirectory / L"backups/quest-reward-reset" / Timestamp();
	fs::create_directories(backupRoot, error);
	if (error) {
		context->LogError("Quest Reward Reset: could not create the backup folder; files were not changed.");
		return;
	}
	for (const auto& [relative, bytes] : desired) {
		(void)bytes;
		const auto original = modRoot / relative;
		if (!fs::exists(original, error)) {
			if (error) {
				context->LogError("Quest Reward Reset: could not inspect an install target; files were not changed.");
				return;
			}
			continue;
		}
		if (!fs::is_regular_file(original, error) || error) {
			context->LogError("Quest Reward Reset: an install target is not a regular file; files were not changed.");
			return;
		}
		const auto backup = backupRoot / relative;
		fs::create_directories(backup.parent_path(), error);
		if (error || !fs::copy_file(original, backup, fs::copy_options::none, error) || error) {
			context->LogError("Quest Reward Reset: a backup could not be created; files were not changed.");
			return;
		}
	}

	std::size_t changedFiles = 0;
	for (const auto& [relative, bytes] : desired) {
		const auto target = modRoot / relative;
		bool same = false;
		const bool targetExists = fs::exists(target, error);
		if (error) {
			context->LogError("Quest Reward Reset: could not inspect an install target; TOML was not created.");
			return;
		}
		if (targetExists) {
			if (!fs::is_regular_file(target, error) || error) {
				context->LogError("Quest Reward Reset: an install target is not a regular file; TOML was not created.");
				return;
			}
			same = ReadFile(target) == bytes;
		}
		if (!same) {
			WriteAtomic(target, bytes);
			++changedFiles;
		}
	}

	const std::string marker =
		"[installation]\n"
		"complete = true\n"
		"plugin_version = \"1.0.24\"\n";
	WriteAtomic(configPath, StringToBytes(marker));
	char message[192] {};
	_snprintf_s(message, sizeof(message), _TRUNCATE,
		"Quest Reward Reset: first-run install completed; %zu mod files updated and a timestamped backup was created.", changedFiles);
	context->LogInfo(message);
}

} // namespace

void RunFirstRunInstall(const D2RL::PluginContext* context) noexcept {
	if (context == nullptr) {
		return;
	}
	try {
		Install(context);
	} catch (const std::exception&) {
		context->LogError("Quest Reward Reset: first-run installation failed; no TOML marker was created, so it will retry next startup.");
	} catch (...) {
		context->LogError("Quest Reward Reset: first-run installation failed unexpectedly; no TOML marker was created, so it will retry next startup.");
	}
}

} // namespace QuestRewardReset
