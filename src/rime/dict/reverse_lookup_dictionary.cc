//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2012-01-05 GONG Chen <chen.sst@gmail.com>
// 2014-07-06 GONG Chen <chen.sst@gmail.com> redesigned binary file format.
//
#include <cfloat>
#include <cstdlib>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include <rime/resource.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/dict/db_pool_impl.h>
#include <rime/dict/dict_settings.h>
#include <rime/dict/reverse_lookup_dictionary.h>

namespace rime {

const char kReverseFormat[] = "Rime::Reverse/3.1";
const double kReverseFormatCompatible = 3.0;

const char kReverseFormatPrefix[] = "Rime::Reverse/";
const size_t kReverseFormatPrefixLen = sizeof(kReverseFormatPrefix) - 1;

static const char* kStemKeySuffix = "\x1fstem";

ReverseDb::ReverseDb(const path& file_path) : MappedFile(file_path) {}

bool ReverseDb::Load() {
  LOG(INFO) << "loading reversedb: " << file_path();

  if (IsOpen())
    Close();

  if (!OpenReadOnly()) {
    LOG(ERROR) << "Error opening reversedb '" << file_path() << "'.";
    return false;
  }

  metadata_ = Find<reverse::Metadata>(0);
  if (!metadata_) {
    LOG(ERROR) << "metadata not found.";
    Close();
    return false;
  }
  if (strncmp(metadata_->format, kReverseFormatPrefix,
              kReverseFormatPrefixLen)) {
    LOG(ERROR) << "invalid metadata.";
    Close();
    return false;
  }
  double format = std::atof(&metadata_->format[kReverseFormatPrefixLen]);
  if (format - kReverseFormatCompatible < 0.0 - DBL_EPSILON ||
      format - kReverseFormatCompatible > 1.0 + DBL_EPSILON) {
    LOG(ERROR) << "incompatible reversedb format.";
    Close();
    return false;
  }

  key_trie_.reset(
      new StringTable(metadata_->key_trie.get(), metadata_->key_trie_size));
  value_trie_.reset(
      new StringTable(metadata_->value_trie.get(), metadata_->value_trie_size));

  return true;
}

bool ReverseDb::Lookup(const string& text, string* result) {
  if (!key_trie_ || !value_trie_ || !metadata_->index.size) {
    return false;
  }
  StringId key_id = key_trie_->Lookup(text);
  if (key_id == kInvalidStringId) {
    return false;
  }
  StringId value_id = metadata_->index.at[key_id];
  *result = value_trie_->GetString(value_id);
  return !result->empty();
}

bool ReverseDb::Build(DictSettings* settings,
                      const Syllabary& syllabary,
                      const Vocabulary& vocabulary,
                      const ReverseLookupTable& stems,
                      uint32_t dict_file_checksum) {
  LOG(INFO) << "building reversedb...";
  ReverseLookupTable rev_table;
  size_t num_syllables = syllabary.size();
  vector<string> id_to_syllable(num_syllables);
  SyllableId syllable_id = 0;
  for (const string& syllable : syllabary) {
    id_to_syllable[syllable_id++] = syllable;
  }
  std::queue<const Vocabulary*> vocabulary_queue;
  vocabulary_queue.push(&vocabulary);
  while (!vocabulary_queue.empty()) {
    const auto& curr_vocabulary = *(vocabulary_queue.front());
    for (const auto& v : curr_vocabulary) {
      const auto& curr_vocabulary_page = v.second;
      for (const auto& curr_entry : curr_vocabulary_page.entries) {
        vector<string> curr_syllables;
        for (const auto& curr_syllable_id : curr_entry->code) {
          if (curr_syllable_id < num_syllables)
            curr_syllables.push_back(id_to_syllable[curr_syllable_id]);
        }
        rev_table[curr_entry->text].insert(boost::algorithm::join(curr_syllables, " "));
      }
      const auto& curr_next_level = curr_vocabulary_page.next_level;
      if (curr_next_level)
        vocabulary_queue.push(curr_next_level.get());
    }
    vocabulary_queue.pop();
  }
  StringTableBuilder key_trie_builder;
  StringTableBuilder value_trie_builder;
  size_t entry_count = rev_table.size() + stems.size();
  vector<StringId> key_ids(entry_count);
  vector<StringId> value_ids(entry_count);
  int i = 0;
  // save reverse lookup entries
  for (const auto& v : rev_table) {
    const string& key(v.first);
    string value(boost::algorithm::join(v.second, " | "));
    key_trie_builder.Add(key, 0.0, &key_ids[i]);
    value_trie_builder.Add(value, 0.0, &value_ids[i]);
    ++i;
  }
  // save stems
  for (const auto& v : stems) {
    string key(v.first + kStemKeySuffix);
    string value(boost::algorithm::join(v.second, " "));
    key_trie_builder.Add(key, 0.0, &key_ids[i]);
    value_trie_builder.Add(value, 0.0, &value_ids[i]);
    ++i;
  }
  key_trie_builder.Build();
  value_trie_builder.Build();

  // dict settings required by UniTE
  string dict_settings;
  if (settings && settings->use_rule_based_encoder()) {
    std::ostringstream yaml;
    settings->SaveToStream(yaml);
    dict_settings = yaml.str();
  }

  // creating reversedb file
  const size_t kReservedSize = 1024;
  size_t key_trie_image_size = key_trie_builder.BinarySize();
  size_t value_trie_image_size = value_trie_builder.BinarySize();
  size_t estimated_data_size = kReservedSize + dict_settings.length() +
                               entry_count * sizeof(StringId) +
                               key_trie_image_size + value_trie_image_size;
  if (!Create(estimated_data_size)) {
    LOG(ERROR) << "Error creating prism file '" << file_path() << "'.";
    return false;
  }

  // create metadata
  metadata_ = Allocate<reverse::Metadata>();
  if (!metadata_) {
    LOG(ERROR) << "Error creating metadata in file '" << file_path() << "'.";
    return false;
  }
  metadata_->dict_file_checksum = dict_file_checksum;
  if (!dict_settings.empty()) {
    if (!CopyString(dict_settings, &metadata_->dict_settings)) {
      LOG(ERROR) << "Error saving dict settings.";
      return false;
    }
  }

  auto entries = Allocate<StringId>(entry_count);
  if (!entries) {
    return false;
  }
  for (size_t i = 0; i < entry_count; ++i) {
    entries[key_ids[i]] = value_ids[i];
  }
  metadata_->index.size = entry_count;
  metadata_->index.at = entries;

  // save key trie image
  char* key_trie_image = Allocate<char>(key_trie_image_size);
  if (!key_trie_image) {
    LOG(ERROR) << "Error creating key trie image.";
    return false;
  }
  key_trie_builder.Dump(key_trie_image, key_trie_image_size);
  metadata_->key_trie = key_trie_image;
  metadata_->key_trie_size = key_trie_image_size;

  // save value trie image
  char* value_trie_image = Allocate<char>(value_trie_image_size);
  if (!value_trie_image) {
    LOG(ERROR) << "Error creating value trie image.";
    return false;
  }
  value_trie_builder.Dump(value_trie_image, value_trie_image_size);
  metadata_->value_trie = value_trie_image;
  metadata_->value_trie_size = value_trie_image_size;

  // at last, complete the metadata
  std::strncpy(metadata_->format, kReverseFormat,
               reverse::Metadata::kFormatMaxLength);
  return true;
}

bool ReverseDb::Save() {
  LOG(INFO) << "saving reverse file: " << file_path();
  return ShrinkToFit();
}

uint32_t ReverseDb::dict_file_checksum() const {
  return metadata_ ? metadata_->dict_file_checksum : 0;
}

ReverseLookupDictionary::ReverseLookupDictionary(an<ReverseDb> db) : db_(db) {}

bool ReverseLookupDictionary::Load() {
  return db_ && (db_->IsOpen() || db_->Load());
}

bool ReverseLookupDictionary::ReverseLookup(const string& text,
                                            string* result) {
  return db_->Lookup(text, result);
}

bool ReverseLookupDictionary::LookupStems(const string& text, string* result) {
  return db_->Lookup(text + kStemKeySuffix, result);
}

an<DictSettings> ReverseLookupDictionary::GetDictSettings() {
  an<DictSettings> settings;
  reverse::Metadata* metadata = db_->metadata();
  if (metadata && !metadata->dict_settings.empty()) {
    string yaml(metadata->dict_settings.c_str());
    std::istringstream iss(yaml);
    settings = New<DictSettings>();
    if (!settings->LoadFromStream(iss)) {
      settings.reset();
    }
  }
  return settings;
}

static const ResourceType kReverseDbResourceType = {"reverse_db", "",
                                                    ".reverse.bin"};

ReverseLookupDictionaryComponent::ReverseLookupDictionaryComponent()
    : DbPool(the<ResourceResolver>(
          Service::instance().CreateDeployedResourceResolver(
              kReverseDbResourceType))) {}

ReverseLookupDictionary* ReverseLookupDictionaryComponent::Create(
    const string& dict_name) {
  auto db = GetDb(dict_name);
  return new ReverseLookupDictionary(db);
};

ReverseLookupDictionary* ReverseLookupDictionaryComponent::Create(
    const Ticket& ticket) {
  if (!ticket.schema)
    return NULL;
  Config* config = ticket.schema->config();
  string dict_name;
  if (!config->GetString(ticket.name_space + "/dictionary", &dict_name)) {
    // missing!
    return NULL;
  }
  return Create(dict_name);
}

}  // namespace rime
