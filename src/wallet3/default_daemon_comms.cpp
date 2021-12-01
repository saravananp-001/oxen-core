#include "default_daemon_comms.hpp"

#include "wallet.hpp"
#include "wallet2½.hpp"
#include "block.hpp"
#include "block_tx.hpp"

#include <common/string_util.h>
#include <epee/misc_log_ex.h>

#include <iostream>

namespace wallet
{
  void
  DefaultDaemonComms::on_get_blocks_response(std::vector<std::string> response)
  {
    if (not response.size())
    {
      std::cout << "on_get_blocks_response(): empty get_blocks response\n";
      //TODO: error handling
      return;
    }
    std::cout << "on_get_blocks_response() got " << response.size() - 1 << " blocks.\n";

    const auto& status = response[0];
    if (status != "OK" and status != "END")
    {
      std::cout << "get_blocks response: " << response[0] << "\n";
      //TODO: error handling
      return;
    }

    // "OK" response with no blocks may mean we requested blocks past the end of the chain
    // TODO: decide/confirm this behavior on the daemon side of things
    if (response.size() == 1)
    {
      std::cout << "get_blocks response.size() == 1\n";
      return;
    }

    std::vector<Block> blocks;
    try
    {
      auto itr = response.cbegin();
      itr++;
      while( itr != response.cend())
      {
        const auto& block_str = *itr;
        auto block_dict = oxenmq::bt_dict_consumer{block_str};

        Block& b = blocks.emplace_back();

        if (block_dict.key() != "hash")
          return;
        b.hash = tools::make_from_guts<crypto::hash>(block_dict.consume_string_view());

        if (block_dict.key() != "height")
          return;
        b.height = block_dict.consume_integer<int64_t>();

        if (block_dict.key() != "timestamp")
          return;
        b.timestamp = block_dict.consume_integer<int64_t>();

        if (block_dict.key() != "transactions")
          return;
        auto txs_list = block_dict.consume_list_consumer();

        while (not txs_list.is_finished())
        {
          if (not txs_list.is_dict())
            return;

          BlockTX tx;

          auto tx_dict = txs_list.consume_dict_consumer();

          if (tx_dict.key() != "global_indices")
            return;
          tx.global_indices = tx_dict.consume_list<std::vector<int64_t> >();

          if (tx_dict.key() != "hash")
            return;
          tx.hash = tools::make_from_guts<crypto::hash>(tx_dict.consume_string_view());

          if (tx_dict.key() != "tx")
            return;

          tx.tx = wallet2½::tx_from_blob(tx_dict.consume_string_view());

          if (not tx_dict.is_finished())
            return;

          b.transactions.push_back(tx);
        }

        if (not block_dict.is_finished())
          return;

        itr++;
      }
    }
    catch (const std::exception& e)
    {
      std::cout << e.what() << "\n";
      return;
    }

    if (blocks.size() == 0)
    {
      std::cout << "received no blocks, but server said response OK\n";
      return;
    }

    int64_t start_height = blocks.front().height;
    int64_t end_height = blocks.back().height;
    std::cout << "on_get_blocks_response() got blocks [" << start_height << " to " << end_height << "]\n";

    if (status == "END")
    {
      std::cout << "Finished syncing wallets, height: " << end_height << "\n";
      omq->job([this](){ syncing = false; }, sync_thread);
    }
    else
    {
      omq->job([this,start_height,end_height](){got_blocks(start_height, end_height);}, sync_thread);
    }

    omq->job([blocks=std::move(blocks),this](){
        for_each_wallet([&](std::shared_ptr<Wallet> wallet){
            wallet->add_blocks(blocks);
            });
        }, sync_thread);

  }

  void
  DefaultDaemonComms::request_top_block_info()
  {
    auto timeout_job = [self=weak_from_this()](){
      if (auto comms = self.lock())
        comms->request_top_block_info();
    };

    omq->cancel_timer(status_timer);
    if (top_block_height == 0)
    {
      omq->add_timer(status_timer, timeout_job, 3s);
    }
    else
      omq->add_timer(status_timer, timeout_job, 15s);

    omq->request(conn, "rpc.get_height",
        [this](bool ok, std::vector<std::string> response)
        {
          if (not ok or response.size() != 2 or response[0] != "200")
            return;

          oxenmq::bt_dict_consumer dc{response[1]};

          int64_t new_height = 0;
          crypto::hash new_hash;

          if (not dc.skip_until("hash"))
            throw std::runtime_error("bad response from rpc.get_height, key 'hash' missing");
          new_hash = tools::make_from_guts<crypto::hash>(dc.consume_string_view());

          if (not dc.skip_until("height"))
            throw std::runtime_error("bad response from rpc.get_height, key 'height' missing");
          new_height = dc.consume_integer<int64_t>();

          top_block_hash = new_hash;

          // RPC response is chain length, not top height
          top_block_height = new_height - 1;
        }, "de");

    omq->request(conn, "rpc.get_fee_estimate",
        [this](bool ok, std::vector<std::string> response)
        {
          if (not ok or response.size() != 2 or response[0] != "200")
            return;

          oxenmq::bt_dict_consumer dc{response[1]};

          int64_t new_fee_per_byte = 0;
          int64_t new_fee_per_output = 0;

          if (not dc.skip_until("fee_per_byte"))
            throw std::runtime_error("bad response from rpc.get_fee_estimate, key 'fee_per_byte' missing");
          new_fee_per_byte = dc.consume_integer<int64_t>();

          if (not dc.skip_until("fee_per_output"))
            throw std::runtime_error("bad response from rpc.get_fee_estimate, key 'fee_per_output' missing");
          new_fee_per_output = dc.consume_integer<int64_t>();

          fee_per_byte = new_fee_per_byte;
          fee_per_output = new_fee_per_output;

        }, "de");
  }

  DefaultDaemonComms::DefaultDaemonComms(std::shared_ptr<oxenmq::OxenMQ> omq)
    : omq(omq),
      sync_thread(omq->add_tagged_thread("sync"))
  {
    omq->MAX_MSG_SIZE = max_response_size;
  }

  void
  DefaultDaemonComms::set_remote(std::string_view address)
  {
    try
    {
      remote = oxenmq::address{address};
    }
    catch (...)
    {
      //TODO: handle this properly
      throw;
    }

    // TODO: proper callbacks
    conn = omq->connect_remote(remote, [](auto){}, [](auto,auto){});

    request_top_block_info();
  }

  void
  DefaultDaemonComms::get_blocks()
  {
    auto req_cb = [this](bool ok, std::vector<std::string> response)
    {
      if (not ok or response.size() == 0)
      {
        //TODO: error logging/handling

        // Retry after a delay to not spam/spin
        auto timer = std::make_shared<oxenmq::TimerID>();
        auto& timer_ref = *timer;
        omq->add_timer(timer_ref, [this,timer=std::move(timer)]{
            omq->cancel_timer(*timer);
            get_blocks();
            },
            500ms,
            true,
            sync_thread);
        return;
      }

      on_get_blocks_response(response);
    };

    std::map<std::string, int64_t> req_params_dict{
      {"max_count", max_sync_blocks},
      {"size_limit", max_response_size},
      {"start_height", sync_from_height}};

    omq->request(conn, "rpc.get_blocks", req_cb, oxenmq::bt_serialize(req_params_dict));
  }

  void
  DefaultDaemonComms::register_wallet(wallet::Wallet& wallet, int64_t height, bool check_sync_height)
  {
    omq->job([this,w=wallet.shared_from_this(),height,check_sync_height](){
        wallets.insert_or_assign(w, height);
        if (check_sync_height)
          sync_from_height = std::min(sync_from_height, height);
        start_syncing();
        }, sync_thread);
  }

  std::pair<int64_t, int64_t>
  DefaultDaemonComms::get_fee_parameters()
  {
    return std::make_pair(fee_per_byte,fee_per_output);
  }

  void
  DefaultDaemonComms::deregister_wallet(wallet::Wallet& wallet, std::promise<void>& p)
  {
    omq->job([this,w=wallet.shared_from_this(),&p]() mutable {
          wallets.erase(w);
          w.reset();
          p.set_value();
          auto itr = std::min_element(wallets.begin(), wallets.end(),
              [](const auto& l, const auto& r){ return l.second < r.second; });
          sync_from_height = itr->second;
          std::cout << "deregister_wallet() setting sync_from_height to " << sync_from_height << "\n";
          if (sync_from_height != 0 and sync_from_height == top_block_height)
            syncing = false;
        }, sync_thread);
  }

  void
  DefaultDaemonComms::for_each_wallet(std::function<void(std::shared_ptr<Wallet>)> func)
  {
    for (auto [wallet,h] : wallets)
    {
      func(wallet);
    }
  }

  void
  DefaultDaemonComms::got_blocks(int64_t start_height, int64_t end_height)
  {
    // if we get caught up, or all wallets are removed, no need to request more blocks
    if (not syncing)
      return;

    if (start_height == sync_from_height)
    {
      sync_from_height = end_height + 1;
    }
    get_blocks();
  }

  void
  DefaultDaemonComms::start_syncing()
  {
    if ((not syncing and sync_from_height < top_block_height) or (top_block_height == 0))
    {
      syncing = true;
      get_blocks();
    }
  }

}  // namespace wallet
