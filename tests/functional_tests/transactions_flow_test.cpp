// Copyright (c) 2014-2018, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>
#include <unordered_map>

#include "wallet/wallet2.h"
using namespace cryptonote;

namespace
{
  uint64_t const TEST_FEE = 5000000000; // 5 * 10^9
  uint64_t const TEST_DUST_THRESHOLD = 5000000000; // 5 * 10^9
}

std::string generate_random_wallet_name()
{
  std::stringstream ss;
  ss << boost::uuids::random_generator()();
  return ss.str();
}

inline uint64_t random(const uint64_t max_value) {
  return (uint64_t(rand()) ^
          (uint64_t(rand())<<16) ^
          (uint64_t(rand())<<32) ^
          (uint64_t(rand())<<48)) % max_value;
}

bool do_send_money(tools::wallet2& w1, tools::wallet2& w2, size_t mix_in_factor, uint64_t amount_to_transfer, transaction& tx, size_t parts=1)
{
  CHECK_AND_ASSERT_MES(parts > 0, false, "parts must be > 0");

  std::vector<cryptonote::tx_destination_entry> dsts;
  dsts.reserve(parts);
  uint64_t amount_used = 0;
  uint64_t max_part = amount_to_transfer / parts;

  for (size_t i = 0; i < parts; ++i)
  {
    cryptonote::tx_destination_entry de;
    de.addr = w2.get_account().get_keys().m_account_address;

    if (i < parts - 1)
      de.amount = random(max_part);
    else
      de.amount = amount_to_transfer - amount_used;
    amount_used += de.amount;

    //std::cout << "PARTS (" << amount_to_transfer << ") " << amount_used << " " << de.amount << std::endl;

    dsts.push_back(de);
  }

  try
  {
    std::vector<tools::wallet2::pending_tx> ptx;
    cryptonote::oxen_construct_tx_params tx_params;
    ptx = w1.create_transactions_2(dsts,
                                   mix_in_factor,
                                   0 /*unlock_time*/,
                                   0 /*priority*/,
                                   std::vector<uint8_t>() /*extra_base*/,
                                   0 /*subaddr_account*/,
                                   {} /*subaddr_indices*/,
                                   tx_params);
    for (auto &p: ptx)
      w1.commit_tx(p);
    return true;
  }
  catch (const std::exception&)
  {
    return false;
  }
}

uint64_t get_money_in_first_transfers(const tools::wallet2::transfer_container& incoming_transfers, size_t n_transfers)
{
  uint64_t summ = 0;
  size_t count = 0;
  for (const auto& td : incoming_transfers)
  {
    summ += td.m_tx.vout[td.m_internal_output_index].amount;
    if(++count >= n_transfers)
      return summ;
  }
  return summ;
}

#define FIRST_N_TRANSFERS 10*10

bool transactions_flow_test(std::string& working_folder,
  std::string path_source_wallet,
  std::string path_target_wallet,
  std::string& daemon_addr_a,
  std::string& daemon_addr_b,
  uint64_t amount_to_transfer, size_t mix_in_factor, size_t transactions_count, size_t transactions_per_second)
{
  oxen::log::warning(logcat, "-----------------------STARTING TRANSACTIONS FLOW TEST-----------------------");
  tools::wallet2 w1, w2;
  if(path_source_wallet.empty())
    path_source_wallet = generate_random_wallet_name();

  if(path_target_wallet.empty())
    path_target_wallet = generate_random_wallet_name();


  try
  {
    w1.generate(working_folder + "/" + path_source_wallet, "");
    w2.generate(working_folder + "/" + path_target_wallet, "");
  }
  catch (const std::exception& e)
  {
    oxen::log::error(logcat, "failed to generate wallet: {}", e.what());
    return false;
  }

  w1.init(daemon_addr_a);

  uint64_t blocks_fetched = 0;
  bool received_money;
  bool ok;
  if(!w1.refresh(true, blocks_fetched, received_money, ok))
  {
    oxen::log::error(logcat, "failed to refresh source wallet from {}", daemon_addr_a);
    return false;
  }

  w2.init(daemon_addr_b);

  oxen::log::info(logcat, fmt::format(fg(fmt::terminal_color::green), "Using wallets:\n"
    << "Source:  " << w1.get_account().get_public_address_str(MAINNET) << "\nPath: " << working_folder + "/" + path_source_wallet << "\n"
    << "Target:  " << w2.get_account().get_public_address_str(MAINNET) << "\nPath: " << working_folder + "/" + path_target_wallet);

  //lets do some money
  epee::net_utils::http::http_simple_client http_client;
  rpc::STOP_MINING::request daemon1_req{};
  rpc::STOP_MINING::response daemon1_rsp{};
  bool r = http_client.set_server(daemon_addr_a, std::nullopt) && epee::net_utils::invoke_http_json("/stop_mine", daemon1_req, daemon1_rsp, http_client, std::chrono::seconds(10));
  CHECK_AND_ASSERT_MES(r, false, "failed to stop mining");

  rpc::START_MINING::request daemon_req{};
  rpc::START_MINING::response daemon_rsp{};
  daemon_req.miner_address = w1.get_account().get_public_address_str(MAINNET);
  daemon_req.threads_count = 9;
  r = epee::net_utils::invoke_http_json("/start_mining", daemon_req, daemon_rsp, http_client, std::chrono::seconds(10));
  CHECK_AND_ASSERT_MES(r, false, "failed to start mining getrandom_outs");
  CHECK_AND_ASSERT_MES(daemon_rsp.status == rpc::STATUS_OK, false, "failed to start mining");

  //wait for money, until balance will have enough money
  w1.refresh(true, blocks_fetched, received_money, ok);
  while(w1.unlocked_balance(0, true) < amount_to_transfer)
  {
    std::this_thread::sleep_for(1s);
    w1.refresh(true, blocks_fetched, received_money, ok);
  }

  //lets make a lot of small outs to ourselves
  //since it is not possible to start from transaction that bigger than 20Kb, we gonna make transactions
  //with 500 outs (about 18kb), and we have to wait appropriate count blocks, mined for test wallet
  while(true)
  {
    tools::wallet2::transfer_container incoming_transfers;
    w1.get_transfers(incoming_transfers);
    if(incoming_transfers.size() > FIRST_N_TRANSFERS && get_money_in_first_transfers(incoming_transfers, FIRST_N_TRANSFERS) < w1.unlocked_balance(0, true) )
    {
      //lets go!
      size_t count = 0;
      for (auto& td : incoming_transfers)
      {
        cryptonote::transaction tx_s;
        bool r = do_send_money(w1, w1, 0, td.m_tx.vout[td.m_internal_output_index].amount - TEST_FEE, tx_s, 50);
        CHECK_AND_ASSERT_MES(r, false, "Failed to send starter tx " << get_transaction_hash(tx_s));
        oxen::log::info(logcat, fmt::format(fg(fmt::terminal_color::green), "Starter transaction sent " << get_transaction_hash(tx_s));
        if(++count >= FIRST_N_TRANSFERS)
          break;
      }
      break;
    }else
    {
      std::this_thread::sleep_for(1s);
      w1.refresh(true, blocks_fetched, received_money, ok);
    }
  }
  //do actual transfer
  uint64_t transfered_money = 0;
  uint64_t transfer_size = amount_to_transfer/transactions_count;
  size_t i = 0;
  struct tx_test_entry
  {
    transaction tx;
    size_t m_received_count;
    uint64_t amount_transfered;
  };
  crypto::key_image lst_sent_ki{};
  std::unordered_map<crypto::hash, tx_test_entry> txs;
  for(i = 0; i != transactions_count; i++)
  {
    uint64_t amount_to_tx = (amount_to_transfer - transfered_money) > transfer_size ? transfer_size: (amount_to_transfer - transfered_money);
    while(w1.unlocked_balance(0, true) < amount_to_tx + TEST_FEE)
    {
      std::this_thread::sleep_for(1s);
      oxen::log::warning(logcat, "not enough money, waiting for cashback or mining");
      w1.refresh(true, blocks_fetched, received_money, ok);
    }

    transaction tx;
    /*size_t n_attempts = 0;
    while (!do_send_money(w1, w2, mix_in_factor, amount_to_tx, tx)) {
        n_attempts++;
        std::cout << "failed to transfer money, refresh and try again (attempts=" << n_attempts << ")" << std::endl;
        w1.refresh();
    }*/


    if(!do_send_money(w1, w2, mix_in_factor, amount_to_tx, tx))
    {
      oxen::log::warning(logcat, "failed to transfer money, tx: {}, refresh and try again", get_transaction_hash(tx));
      w1.refresh(true, blocks_fetched, received_money, ok);
      if(!do_send_money(w1, w2, mix_in_factor, amount_to_tx, tx))
      {
        oxen::log::warning(logcat, "failed to transfer money, second chance. tx: {}, exit", get_transaction_hash(tx));
        LOCAL_ASSERT(false);
        return false;
      }
    }
    lst_sent_ki = var::get<txin_to_key>(tx.vin[0]).k_image;

    transfered_money += amount_to_tx;

    oxen::log::warning(logcat, "transferred {}, i={}", amount_to_tx, i);
    tx_test_entry& ent = txs[get_transaction_hash(tx)] = {};
    ent.amount_transfered = amount_to_tx;
    ent.tx = tx;
    //if(i % transactions_per_second)
    //  std::this_thread::sleep_for(1s);
  }


  oxen::log::warning(logcat, "waiting some new blocks...");
  std::this_thread::sleep_for(TARGET_BLOCK_TIME*20*1s);//wait two blocks before sync on another wallet on another daemon
  oxen::log::warning(logcat, "refreshing...");
  bool recvd_money = false;
  while(w2.refresh(true, blocks_fetched, recvd_money, ok) && ( (blocks_fetched && recvd_money) || !blocks_fetched  ) )
  {
    std::this_thread::sleep_for(TARGET_BLOCK_TIME*1s);//wait two blocks before sync on another wallet on another daemon
  }

  uint64_t money_2 = w2.balance(0, true);
  if(money_2 == transfered_money)
  {
    oxen::log::info(logcat, fmt::format(fg(fmt::terminal_color::green), "-----------------------FINISHING TRANSACTIONS FLOW TEST OK-----------------------");
    oxen::log::info(logcat, fmt::format(fg(fmt::terminal_color::green), "transferred {} via {} transactions", print_money(transfered_money), i);
    return true;
  }else
  {
    tools::wallet2::transfer_container tc;
    w2.get_transfers(tc);
    for (auto& td : tc)
    {
      auto it = txs.find(td.m_txid);
      CHECK_AND_ASSERT_MES(it != txs.end(), false, "transaction not found in local cache");
      it->second.m_received_count += 1;
    }

    for (auto& tx_pair : txs)
    {
      if(tx_pair.second.m_received_count != 1)
      {
        oxen::log::error(logcat, "{}{}", , "Transaction lost: ", get_transaction_hash(tx_pair.second.tx));
      }

    }

    oxen::log::error(logcat, "-----------------------FINISHING TRANSACTIONS FLOW TEST FAILED-----------------------" );
    oxen::log::error(logcat, "income {} via {} transactions, expected money = {}", print_money(money_2), i, print_money(transfered_money));
    LOCAL_ASSERT(false);
    return false;
  }

  return true;
}

