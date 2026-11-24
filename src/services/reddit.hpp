#pragma once

// Reddit posts & memes viewer (via meme-api.com JSON relay, SFW filtered).

#include <string>
#include <vector>

namespace reddit {

struct Post {
    std::string title;
    std::string author;
    std::string subreddit;
    std::string image_url;
    std::string thumb_url;
    int ups = 0;
};

struct PostList {
    std::vector<Post> items;
    std::string error;
};

PostList fetch_subreddit(const std::string& subreddit);

}  // namespace reddit

