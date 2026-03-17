#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

struct ThemeMetrics;

class FileBrowserActivity final : public Activity {
 private:
  struct FileMetadata {
    std::string title;
    std::string author;
  };

  // Deletion
  void clearFileMetadata(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::vector<FileMetadata> filesMetadata;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

  void renderList(const ThemeMetrics& metrics, int pageWidth, int pageHeight);
  void renderCoverList(const ThemeMetrics& metrics, int pageWidth, int pageHeight);

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("FileBrowser", renderer, mappedInput), basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
