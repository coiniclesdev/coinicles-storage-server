#include "service_node.h"

#include "Database.hpp"
#include "lokinet_identity.hpp"
#include "utils.hpp"

#include "Item.hpp"
#include "http_connection.h"

#include <chrono>
#include <fstream>
#include <iomanip>

#include <boost/bind.hpp>

#include <boost/log/trivial.hpp>

/// move this out
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "serialization.h"

using service_node::storage::Item;

namespace loki {

void say_hello(const boost::system::error_code& ec) { std::cout << "hello\n"; }

/// TODO: can we reuse context (reset it)?
std::string hash_data(std::string data) {

    unsigned char result[EVP_MAX_MD_SIZE];

    /// Allocate and init digest context
    EVP_MD_CTX* mdctx = EVP_MD_CTX_create();

    /// Set the method
    EVP_DigestInit_ex(mdctx, EVP_sha512(), NULL);

    /// Do the hashing, can be called multiple times (?)
    /// to hash
    EVP_DigestUpdate(mdctx, data.data(), data.size());

    unsigned int md_len;

    EVP_DigestFinal_ex(mdctx, result, &md_len);

    /// Clean up the context
    EVP_MD_CTX_destroy(mdctx);

    /// Not sure if this is needed
    EVP_cleanup();

    /// store into the string
    /// TODO: use binary instead?
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < EVP_MAX_MD_SIZE; i++) {
        ss << std::setw(2) << static_cast<unsigned>(result[i]);
    }

    return std::string(ss.str());
}

ServiceNode::ServiceNode(boost::asio::io_context& ioc, uint16_t port,
                         const std::string& identityPath,
                         const std::string& dbLocation)
    : ioc_(ioc), db_(std::make_unique<Database>(dbLocation)), our_port_(port),
      update_timer_(ioc, std::chrono::milliseconds(100)) {

#ifndef INTEGRATION_TEST
    const std::vector<uint8_t> publicKey =
        parseLokinetIdentityPublic(identityPath);
    char buf[64] = {0};
    std::string our_address;
    if (char const* dest = util::base32z_encode(publicKey, buf)) {
        our_address.append(dest);
        our_address.append(".snode");
    }
    our_address_.address = our_address;
#else
    our_address_.port = port;
#endif

    swarm_timer_tick();
}

ServiceNode::~ServiceNode() = default;

/// make this async
void ServiceNode::relay_one(const message_ptr msg, sn_record_t sn) const {

    /// TODO: need to encrypt messages?

    BOOST_LOG_TRIVIAL(debug) << "Relaying a message to " << to_string(sn);

    request_t req;
    serialize_message(req.body(), *msg);

    req.target("/v1/swarms/push");

    /// TODO: how to handle a failure here?
    make_http_request(ioc_, sn.address, sn.port, req,
                      [](std::shared_ptr<std::string>) {

                      });
}

void ServiceNode::relay_batch(const std::string& data, sn_record_t sn) const {

    BOOST_LOG_TRIVIAL(debug) << "Relaying a batch to " << to_string(sn);

    request_t req;
    req.body() = data;
    req.target("/v1/swarms/push_all");

    make_http_request(ioc_, sn.address, sn.port, req,
                      [](std::shared_ptr<std::string>) {

                      });
}

/// initiate a /swarms/push request
void ServiceNode::push_message(const message_ptr msg) {

    if (!swarm_)
        return;

    auto others = swarm_->other_nodes();

    BOOST_LOG_TRIVIAL(debug)
        << "push_message to " << others.size() << " other nodes";

    for (auto& address : others) {
        /// send a request asynchronously (todo: collect confirmations)
        relay_one(msg, address);
    }
}

/// do this asynchronously on a different thread? (on the same thread?)
bool ServiceNode::process_store(const message_ptr msg) {

    /// TODO: accept messages if they are coming from other service nodes

    /// only accept a message if we are in a swarm
    if (!swarm_) {
        BOOST_LOG_TRIVIAL(error) << "error: my swarm in not initialized";
        return false;
    }

    /// store to the database
    save_if_new(msg);

    /// initiate a /swarms/push request;
    /// (done asynchronously)
    this->push_message(msg);

    return true;
}

void ServiceNode::process_push(const message_ptr msg) { save_if_new(msg); }

void ServiceNode::save_if_new(const message_ptr msg) {

    db_->store(msg->hash_, msg->pk_, msg->text_, msg->ttl_, msg->timestamp_,
               msg->nonce_);

    BOOST_LOG_TRIVIAL(trace) << "saving message: " << msg->text_;
}

void ServiceNode::on_swarm_update(all_swarms_t all_swarms) {
    if (!swarm_) {
        BOOST_LOG_TRIVIAL(trace) << "initialized our swarm";
        swarm_ = std::make_unique<Swarm>(our_address_);
    }

    const SwarmEvents events = swarm_->update_swarms(all_swarms);

    if (!events.new_snodes.empty()) {
        bootstrap_peers(events.new_snodes);
    }

    if (!events.new_swarms.empty()) {
        bootstrap_swarms(events.new_swarms);
    }

    if (events.decommissioned) {
        /// Go through all our PK and push them accordingly
        salvage_data();
    }

    this->purge_outdated();
}

void ServiceNode::swarm_timer_tick() {
    const swarm_callback_t cb = std::bind(&ServiceNode::on_swarm_update, this, std::placeholders::_1);
    request_swarm_update(ioc_, std::move(cb));
    update_timer_.expires_after(std::chrono::seconds(2));
    update_timer_.async_wait(boost::bind(&ServiceNode::swarm_timer_tick, this));
}

void ServiceNode::bootstrap_peers(const std::vector<sn_record_t>& peers) const {

    std::string data = serialize_all();

    for (const sn_record_t& sn : peers) {
        relay_batch(data, sn);
    }
}

template <typename T>
std::string vec_to_string(const std::vector<T>& vec) {

    std::stringstream ss;

    ss << "[";

    for (auto i = 0u; i < vec.size(); ++i) {
        ss << vec[i];

        if (i < vec.size() - 1) {
            ss << " ";
        }
    }

    ss << "]";

    return ss.str();
}

void ServiceNode::bootstrap_swarms(
    const std::vector<swarm_id_t>& swarms) const {

    if (swarms.empty()) {
        BOOST_LOG_TRIVIAL(info) << "bootstrapping all swarms\n";
    } else {
        BOOST_LOG_TRIVIAL(info)
            << "bootstrapping swarms: " << vec_to_string(swarms);
    }

    const auto& all_swarms = swarm_->all_swarms();

    std::vector<Item> all_entries;
    if (!db_->retrieve("", all_entries, "")) {
        BOOST_LOG_TRIVIAL(error)
            << "could not retrieve entries from the database\n";
        return;
    }

    std::unordered_map<swarm_id_t, size_t> swarm_id_to_idx;
    for (auto i = 0u; i < all_swarms.size(); ++i) {
        swarm_id_to_idx.insert({all_swarms[i].swarm_id, i});
    }

    /// See what pubkeys we have
    std::unordered_map<std::string, swarm_id_t> cache;

    BOOST_LOG_TRIVIAL(debug)
        << "we have " << all_entries.size() << " messages\n";

    std::unordered_map<swarm_id_t, std::vector<message_t>> to_relay;

    for (auto& entry : all_entries) {

        swarm_id_t swarm_id;
        const auto it = cache.find(entry.pubKey);
        if (it == cache.end()) {
            swarm_id = get_swarm_by_pk(all_swarms, entry.pubKey);
            cache.insert({entry.pubKey, swarm_id});
        } else {
            swarm_id = it->second;
        }

        bool relevant = false;
        for (const auto swarm : swarms) {

            if (swarm == swarm_id) {
                relevant = true;
            }
        }

        if (relevant || swarms.empty()) {

            to_relay[swarm_id].emplace_back(entry.pubKey, entry.bytes,
                                            entry.hash, entry.ttl,
                                            entry.timestamp, entry.nonce);
        }
    }

    BOOST_LOG_TRIVIAL(trace)
        << "Bootstrapping " << to_relay.size() << " swarms";

    for (const auto& kv : to_relay) {
        const uint64_t swarm_id = kv.first;
        /// what if not found?
        const size_t idx = swarm_id_to_idx[swarm_id];

        const std::vector<std::string> data = serialize_messages(kv.second);

        BOOST_LOG_TRIVIAL(info) << "serialized batches: " << data.size();

        for (const sn_record_t& sn : all_swarms[idx].snodes) {
            // TODO: use a constructor from Item to message_t?
            for (const std::string& batch : data) {
                relay_batch(batch, sn);
            }
        }
    }
}

void ServiceNode::salvage_data() const {

    /// This is very similar to ServiceNode::bootstrap_swarms, so might reuse it
    bootstrap_swarms({});
}

bool ServiceNode::retrieve(const std::string& pubKey,
                           const std::string& last_hash,
                           std::vector<Item>& items) {
    return db_->retrieve(pubKey, items, last_hash);
}

bool ServiceNode::get_all_messages(std::vector<Item>& all_entries) {

    BOOST_LOG_TRIVIAL(trace) << "get all messages";

    return db_->retrieve("", all_entries, "");
}

std::string ServiceNode::serialize_all() const {

    std::vector<Item> all_entries;
    db_->retrieve("", all_entries, "");

    std::string result;

    for (auto& entry : all_entries) {

        result += serialize_message(entry);
    }

    return result;
}

void ServiceNode::purge_outdated() {

    /// TODO: use database instead, for now it is a no-op
    return;
}

void ServiceNode::process_push_all(std::shared_ptr<std::string> blob) {

    /// This should already be checked, but just to be sure
    if (!blob || *blob == "")
        return;

    if (*blob == "")
        return;

    std::vector<message_t> messages = deserialize_messages(*blob);

    BOOST_LOG_TRIVIAL(trace) << "saving all: begin";

    BOOST_LOG_TRIVIAL(debug) << "got " << messages.size()
                             << " messages form peers, size: " << blob->size();

    for (auto& msg : messages) {

        /// shouldn't have to create shared ptr here...
        // TODO: Actually use the message values here
        save_if_new(std::make_shared<message_t>(msg));
    }

    BOOST_LOG_TRIVIAL(trace) << "saving all: end";
}

bool ServiceNode::is_pubkey_for_us(const std::string& pk) const {
    const auto& all_swarms = swarm_->all_swarms();
    return swarm_->is_pubkey_for_us(all_swarms, pk);
}

std::vector<sn_record_t> ServiceNode::get_snodes_by_pk(const std::string& pk) {

    const auto& all_swarms = swarm_->all_swarms();

    swarm_id_t swarm_id = get_swarm_by_pk(all_swarms, pk);

    // TODO: have get_swarm_by_pk return idx into all_swarms instead,
    // so we don't have to find it again

    for (const auto& si : all_swarms) {
        if (si.swarm_id == swarm_id)
            return si.snodes;
    }

    BOOST_LOG_TRIVIAL(fatal) << "Something went wrong in get_snodes_by_pk";

    return {};
}

} // namespace loki