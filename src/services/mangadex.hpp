#pragma once

// MangaDex API v5 client (popular/search manga, FR/EN chapters, page URLs).

#include <string>
#include <vector>

namespace mangadex {

struct Manga {
    std::string id;
    std::string title;
    std::string cover_url;
    std::string description;
};

struct MangaList {
    std::vector<Manga> items;
    std::string error;
};

struct Chapter {
    std::string id;
    std::string number;
    std::string title;
    std::string lang;
};

struct ChapterList {
    std::vector<Chapter> items;
    std::string error;
};

struct PageList {
    std::vector<std::string> urls;
    std::string error;
};

MangaList browse_popular();
MangaList search(const std::string& query);
ChapterList get_chapters(const std::string& manga_id);
PageList get_pages(const std::string& chapter_id);

}  // namespace mangadex

