#include <set>
#include <tuple>

#include "lgraph/lgraph.h"
#include "snb_common.h"
#include "snb_constants.h"
#include "tsl/hopscotch_set.h"

void ProcessFriendMessages(lgraph_api::VertexIterator& person_friend, lgraph_api::VertexIterator& message,
                           std::set<std::tuple<int64_t, int64_t, int64_t, int64_t> >& candidates,
                           const int64_t max_date, const size_t limit_results) {
    for (auto friend_posts = lgraph_api::LabeledInEdgeIterator(person_friend, POSTHASCREATOR); friend_posts.IsValid();
         friend_posts.Next()) {
        int64_t creation_date = friend_posts[POSTHASCREATOR_CREATIONDATE].integer();
        if (creation_date > max_date) continue;
        int64_t message_vid = -1;
        int64_t message_id;
        if (candidates.size() >= limit_results) {
            auto& candidate = *candidates.rbegin();
            if (0 - creation_date > std::get<0>(candidate)) continue;
            if (0 - creation_date == std::get<0>(candidate)) {
                message_vid = friend_posts.GetSrc();
                message.Goto(message_vid);
                message_id = message[POST_ID].integer();
                if (message_id > std::get<1>(candidate)) continue;
            }
        }
        if (message_vid == -1) {
            message_vid = friend_posts.GetSrc();
            message.Goto(message_vid);
            message_id = message[POST_ID].integer();
        }
        candidates.emplace(0 - creation_date, message_id, person_friend.GetId(), message_vid);
        if (candidates.size() > limit_results) {
            candidates.erase(--candidates.end());
        }
    }
    for (auto friend_comments = lgraph_api::LabeledInEdgeIterator(person_friend, COMMENTHASCREATOR);
         friend_comments.IsValid(); friend_comments.Next()) {
        int64_t creation_date = friend_comments[COMMENTHASCREATOR_CREATIONDATE].integer();
        if (creation_date > max_date) continue;
        int64_t message_vid = -1;
        int64_t message_id;
        if (candidates.size() >= limit_results) {
            auto& candidate = *candidates.rbegin();
            if (0 - creation_date > std::get<0>(candidate)) continue;
            if (0 - creation_date == std::get<0>(candidate)) {
                message_vid = friend_comments.GetSrc();
                message.Goto(message_vid);
                message_id = message[COMMENT_ID].integer();
                if (message_id > std::get<1>(candidate)) continue;
            }
        }
        if (message_vid == -1) {
            message_vid = friend_comments.GetSrc();
            message.Goto(message_vid);
            message_id = message[COMMENT_ID].integer();
        }
        candidates.emplace(0 - creation_date, message_id, person_friend.GetId(), 0 - message_vid);
        if (candidates.size() > limit_results) {
            candidates.erase(--candidates.end());
        }
    }
}

extern "C" bool Process(lgraph_api::GraphDB& db, const std::string& request, std::string& response) {
    constexpr size_t limit_results = 20;

    std::string input = lgraph_api::base64::Decode(request);
    std::stringstream iss(input);
    int64_t person_id = ReadInt64(iss);
    int64_t max_date = ReadInt64(iss);

    auto txn = db.CreateReadTxn();
    int64_t start_vid;
    {
        auto fd = lgraph_api::FieldData::Int64(person_id);
        auto iit = txn.GetVertexIndexIterator(PERSON, PERSON_ID, fd, fd);
        start_vid = iit.GetVid();
    }
    auto message = txn.GetVertexIterator();
    // 类似IC8，也是用set实现topk
    std::set<std::tuple<int64_t, int64_t, int64_t, int64_t> > candidates;
    // KEY_NOTE
    // 两层BFS实现person-knows*1..2-friend: visited
    tsl::hopscotch_set<int64_t> visited({start_vid});
    // KEY_NOTE
    // visit的实现：hopscotch_set而不是unordered_set
    // see https://github.com/Tessil/hopscotch-map and https://tessil.github.io/hopscotch-map/classtsl_1_1hopscotch__set.html
    // The hopscotch-map library is a C++ implementation of a fast hash map and hash set using open-addressing and hopscotch hashing to resolve collisions. 
    // It is a cache-friendly data structure offering better performances than std::unordered_map in most cases 
    // and is closely similar to google::dense_hash_map while using less memory and providing more functionalities.
    // A benchmark of tsl::hopscotch_map against other hash maps may be found here: https://tessil.github.io/2016/08/29/benchmark-hopscotch-map.html
    // An overview of hopscotch hashing and some implementation details can be found here: https://tessil.github.io/2016/08/29/hopscotch-hashing.html
    std::vector<int64_t> curr_frontier({start_vid});
    auto person = txn.GetVertexIterator();
    for (int hop = 0; hop <= 2; hop++) {
        std::vector<int64_t> next_frontier;
        for (auto vid : curr_frontier) {
            person.Goto(vid);
            if (hop < 2) {
                for (auto person_friends = lgraph_api::LabeledOutEdgeIterator(person, KNOWS); person_friends.IsValid();
                     person_friends.Next()) {
                    int64_t friend_vid = person_friends.GetDst();
                    if (visited.find(friend_vid) == visited.end()) {
                        visited.emplace(friend_vid);
                        next_frontier.emplace_back(friend_vid);
                    }
                }
                for (auto person_friends = lgraph_api::LabeledInEdgeIterator(person, KNOWS); person_friends.IsValid();
                     person_friends.Next()) {
                    int64_t friend_vid = person_friends.GetSrc();
                    if (visited.find(friend_vid) == visited.end()) {
                        visited.emplace(friend_vid);
                        next_frontier.emplace_back(friend_vid);
                    }
                }
            }
            if (hop == 0) continue;
            ProcessFriendMessages(person, message, candidates, max_date, limit_results);
        }
        std::sort(next_frontier.begin(), next_frontier.end());
        curr_frontier.swap(next_frontier);
    }
    std::stringstream oss;
    WriteInt16(oss, candidates.size());
    for (auto& tup : candidates) {
        person.Goto(std::get<2>(tup));
        WriteInt64(oss, person[PERSON_ID].integer());
        WriteString(oss, person[PERSON_FIRSTNAME].string());
        WriteString(oss, person[PERSON_LASTNAME].string());
        WriteInt64(oss, std::get<1>(tup));
        if (std::get<3>(tup) >= 0) {
            message.Goto(std::get<3>(tup));
            auto content = message[POST_CONTENT];
            WriteString(oss, content.is_null() ? message[POST_IMAGEFILE].string() : content.string());
        } else {
            message.Goto(-std::get<3>(tup));
            WriteString(oss, message[COMMENT_CONTENT].string());
        }
        WriteInt64(oss, 0 - std::get<0>(tup));
    }
    response = oss.str();
    return true;
}
