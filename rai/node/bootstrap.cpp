#include <rai/node/bootstrap.hpp>

#include <rai/node/common.hpp>
#include <rai/node/node.hpp>

#include <boost/log/trivial.hpp>

rai::block_synchronization::block_synchronization (boost::log::sources::logger_mt & log_a, std::function <void (rai::transaction &, rai::block const &)> const & target_a, rai::block_store & store_a) :
log (log_a),
target (target_a),
store (store_a)
{
}

rai::block_synchronization::~block_synchronization ()
{
}

namespace {
class add_dependency_visitor : public rai::block_visitor
{
public:
    add_dependency_visitor (rai::transaction & transaction_a, rai::block_synchronization & sync_a) :
	transaction (transaction_a),
    sync (sync_a),
    result (true)
    {
    }
    void send_block (rai::send_block const & block_a) override
    {
        add_dependency (block_a.hashables.previous);
    }
    void receive_block (rai::receive_block const & block_a) override
    {
        add_dependency (block_a.hashables.previous);
        if (result)
        {
            add_dependency (block_a.hashables.source);
        }
    }
    void open_block (rai::open_block const & block_a) override
    {
        add_dependency (block_a.hashables.source);
    }
    void change_block (rai::change_block const & block_a) override
    {
        add_dependency (block_a.hashables.previous);
    }
    void add_dependency (rai::block_hash const & hash_a)
    {
        if (!sync.synchronized (transaction, hash_a))
        {
            result = false;
            sync.blocks.push (hash_a);
        }
		else
		{
			// Block is already synchronized, normal
		}
    }
	rai::transaction & transaction;
    rai::block_synchronization & sync;
    bool result;
};
}

bool rai::block_synchronization::add_dependency (rai::transaction & transaction_a, rai::block const & block_a)
{
    add_dependency_visitor visitor (transaction_a, *this);
    block_a.visit (visitor);
    return visitor.result;
}

bool rai::block_synchronization::fill_dependencies (rai::transaction & transaction_a)
{
    auto result (false);
    auto done (false);
    while (!result && !done)
    {
		auto hash (blocks.top ());
        auto block (retrieve (transaction_a, hash));
        if (block != nullptr)
        {
            done = add_dependency (transaction_a, *block);
        }
        else
        {
			BOOST_LOG (log) << boost::str (boost::format ("Unable to retrieve block while generating dependencies %1%") % hash.to_string ());
            result = true;
        }
    }
    return result;
}

bool rai::block_synchronization::synchronize_one (rai::transaction & transaction_a)
{
    auto result (fill_dependencies (transaction_a));
    if (!result)
    {
		auto hash (blocks.top ());
        auto block (retrieve (transaction_a,hash));
        blocks.pop ();
        if (block != nullptr)
        {
			target (transaction_a, *block);
		}
		else
		{
			BOOST_LOG (log) << boost::str (boost::format ("Unable to retrieve block while synchronizing %1%") % hash.to_string ());
			result = true;
		}
    }
    return result;
}

bool rai::block_synchronization::synchronize (rai::transaction & transaction_a, rai::block_hash const & hash_a)
{
    auto result (false);
    blocks.push (hash_a);
    while (!result && !blocks.empty ())
    {
        result = synchronize_one (transaction_a);
    }
    return result;
}

rai::pull_synchronization::pull_synchronization (boost::log::sources::logger_mt & log_a, std::function <void (rai::transaction &, rai::block const &)> const & target_a, rai::block_store & store_a) :
block_synchronization (log_a, target_a, store_a)
{
}

std::unique_ptr <rai::block> rai::pull_synchronization::retrieve (rai::transaction & transaction_a, rai::block_hash const & hash_a)
{
    return store.unchecked_get (transaction_a, hash_a);
}

bool rai::pull_synchronization::synchronized (rai::transaction & transaction_a, rai::block_hash const & hash_a)
{
    return store.block_exists (transaction_a, hash_a);
}

rai::push_synchronization::push_synchronization (boost::log::sources::logger_mt & log_a, std::function <void (rai::transaction &, rai::block const &)> const & target_a, rai::block_store & store_a) :
block_synchronization (log_a, target_a, store_a)
{
}

bool rai::push_synchronization::synchronized (rai::transaction & transaction_a, rai::block_hash const & hash_a)
{
    auto result (!store.unsynced_exists (transaction_a, hash_a));
	if (!result)
	{
		store.unsynced_del (transaction_a, hash_a);
	}
	return result;
}

std::unique_ptr <rai::block> rai::push_synchronization::retrieve (rai::transaction & transaction_a, rai::block_hash const & hash_a)
{
    return store.block_get (transaction_a, hash_a);
}

rai::bootstrap_client::bootstrap_client (std::shared_ptr <rai::node> node_a, std::shared_ptr <rai::bootstrap_attempt> attempt_a) :
node (node_a),
attempt (attempt_a),
socket (node_a->network.service)
{
}

rai::bootstrap_client::~bootstrap_client ()
{
	if (node->config.logging.network_logging ())
	{
		BOOST_LOG (node->log) << "Exiting bootstrap client";
	}
}

void rai::bootstrap_client::run (boost::asio::ip::tcp::endpoint const & endpoint_a)
{
    if (node->config.logging.network_logging ())
    {
        BOOST_LOG (node->log) << boost::str (boost::format ("Initiating bootstrap connection to %1%") % endpoint_a);
    }
    auto this_l (shared_from_this ());
    socket.async_connect (endpoint_a, [this_l, endpoint_a] (boost::system::error_code const & ec)
    {
		if (!ec)
		{
			if (!this_l->attempt->connected.exchange (true))
			{
				this_l->connect_action ();
			}
			else
			{
				BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Disconnecting from: %1% because bootstrap in progress") % endpoint_a);
			}
		}
		else
		{
			if (this_l->node->config.logging.network_logging ())
			{
				BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Error initiating bootstrap connection %1%") % ec.message ());
			}
			this_l->node->peers.bootstrap_failed (rai::endpoint (endpoint_a.address (), endpoint_a.port ()));
		}
    });
}

void rai::bootstrap_client::connect_action ()
{
	std::unique_ptr <rai::frontier_req> request (new rai::frontier_req);
	request->start.clear ();
	request->age = std::numeric_limits <decltype (request->age)>::max ();
	request->count = std::numeric_limits <decltype (request->age)>::max ();
	auto send_buffer (std::make_shared <std::vector <uint8_t>> ());
	{
		rai::vectorstream stream (*send_buffer);
		request->serialize (stream);
	}
	if (node->config.logging.network_logging ())
	{
		BOOST_LOG (node->log) << boost::str (boost::format ("Initiating frontier request for %1% age %2% count %3%") % request->start.to_string () % request->age % request->count);
	}
	auto this_l (shared_from_this ());
	boost::asio::async_write (socket, boost::asio::buffer (send_buffer->data (), send_buffer->size ()), [this_l, send_buffer] (boost::system::error_code const & ec, size_t size_a)
	{
		this_l->sent_request (ec, size_a);
	});
}

void rai::bootstrap_client::sent_request (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        auto this_l (shared_from_this ());
        auto client_l (std::make_shared <rai::frontier_req_client> (this_l));
        client_l->receive_frontier ();
    }
    else
    {
        if (node->config.logging.network_logging ())
        {
            BOOST_LOG (node->log) << boost::str (boost::format ("Error while sending bootstrap request %1%") % ec.message ());
        }
    }
}

rai::frontier_req_client::frontier_req_client (std::shared_ptr <rai::bootstrap_client> const & connection_a) :
connection (connection_a),
current (0)
{
	next ();
}

rai::frontier_req_client::~frontier_req_client ()
{
    if (connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->node->log) << "Exiting frontier_req initiator";
    }
}

void rai::frontier_req_client::receive_frontier ()
{
    auto this_l (shared_from_this ());
    boost::asio::async_read (connection->socket, boost::asio::buffer (receive_buffer.data (), sizeof (rai::uint256_union) + sizeof (rai::uint256_union)), [this_l] (boost::system::error_code const & ec, size_t size_a)
    {
        this_l->received_frontier (ec, size_a);
    });
}

void rai::frontier_req_client::request_account (rai::account const & account_a)
{
    // Account they know about and we don't.
    pulls [account_a] = rai::block_hash (0);
}

void rai::frontier_req_client::completed_pulls ()
{
    auto this_l (shared_from_this ());
    auto pushes (std::make_shared <rai::bulk_push_client> (this_l));
    pushes->start ();
}

void rai::frontier_req_client::unsynced (MDB_txn * transaction_a, rai::block_hash const & ours_a, rai::block_hash const & theirs_a)
{
	auto current (ours_a);
	while (!current.is_zero () && current != theirs_a)
	{
		connection->node->store.unsynced_put (transaction_a, current);
		auto block (connection->node->store.block_get (transaction_a, current));
		current = block->previous ();
	}
}

void rai::frontier_req_client::received_frontier (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        assert (size_a == sizeof (rai::uint256_union) + sizeof (rai::uint256_union));
        rai::account account;
        rai::bufferstream account_stream (receive_buffer.data (), sizeof (rai::uint256_union));
        auto error1 (rai::read (account_stream, account));
        assert (!error1);
        rai::block_hash latest;
        rai::bufferstream latest_stream (receive_buffer.data () + sizeof (rai::uint256_union), sizeof (rai::uint256_union));
        auto error2 (rai::read (latest_stream, latest));
        assert (!error2);
        if (!account.is_zero ())
        {
            while (!current.is_zero () && current < account)
            {
				rai::transaction transaction (connection->node->store.environment, nullptr, true);
                // We know about an account they don't.
				unsynced (transaction, info.head, 0);
				next ();
            }
            if (!current.is_zero ())
            {
                if (account == current)
                {
                    if (latest == info.head)
                    {
                        // In sync
                    }
                    else
					{
						rai::transaction transaction (connection->node->store.environment, nullptr, true);
						if (connection->node->store.block_exists (transaction, latest))
						{
							// We know about a block they don't.
							unsynced (transaction, info.head, latest);
						}
						else
						{
							// They know about a block we don't.
							pulls [account] = info.head;
						}
					}
					next ();
                }
                else
                {
                    assert (account < current);
                    request_account (account);
                }
            }
            else
            {
                request_account (account);
            }
            receive_frontier ();
        }
        else
        {
			{
				rai::transaction transaction (connection->node->store.environment, nullptr, true);
				while (!current.is_zero ())
				{
					// We know about an account they don't.
					unsynced (transaction, info.head, 0);
					next ();
				}
			}
            completed_requests ();
        }
    }
    else
    {
        if (connection->node->config.logging.network_logging ())
        {
            BOOST_LOG (connection->node->log) << boost::str (boost::format ("Error while receiving frontier %1%") % ec.message ());
        }
    }
}

void rai::frontier_req_client::next ()
{
	rai::transaction transaction (connection->node->store.environment, nullptr, false);
	auto iterator (connection->node->store.latest_begin (transaction, rai::uint256_union (current.number () + 1)));
	if (iterator != connection->node->store.latest_end ())
	{
		current = rai::account (iterator->first);
		info = rai::account_info (iterator->second);
	}
	else
	{
		current.clear ();
	}
}

void rai::frontier_req_client::completed_requests ()
{
    auto this_l (shared_from_this ());
    auto pulls (std::make_shared <rai::bulk_pull_client> (this_l));
    pulls->request ();
}

void rai::frontier_req_client::completed_pushes ()
{
}

void rai::bulk_pull_client::request ()
{
    if (current != end)
    {
        rai::bulk_pull req;
        req.start = current->first;
        req.end = current->second;
        ++current;
        auto buffer (std::make_shared <std::vector <uint8_t>> ());
        {
            rai::vectorstream stream (*buffer);
            req.serialize (stream);
        }
		if (connection->connection->node->config.logging.network_logging ())
		{
			BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Requesting account %1% down to %2%") % req.start.to_account () % req.end.to_string ());
		}
        auto this_l (shared_from_this ());
        boost::asio::async_write (connection->connection->socket, boost::asio::buffer (buffer->data (), buffer->size ()), [this_l, buffer] (boost::system::error_code const & ec, size_t size_a)
            {
                if (!ec)
                {
                    this_l->receive_block ();
                }
                else
                {
                    BOOST_LOG (this_l->connection->connection->node->log) << boost::str (boost::format ("Error sending bulk pull request %1%") % ec.message ());
                }
            });
    }
    else
    {
        process_end ();
        connection->completed_pulls ();
    }
}

void rai::bulk_pull_client::receive_block ()
{
    auto this_l (shared_from_this ());
    boost::asio::async_read (connection->connection->socket, boost::asio::buffer (receive_buffer.data (), 1), [this_l] (boost::system::error_code const & ec, size_t size_a)
    {
        if (!ec)
        {
            this_l->received_type ();
        }
        else
        {
            BOOST_LOG (this_l->connection->connection->node->log) << boost::str (boost::format ("Error receiving block type %1%") % ec.message ());
        }
    });
}

void rai::bulk_pull_client::received_type ()
{
    auto this_l (shared_from_this ());
    rai::block_type type (static_cast <rai::block_type> (receive_buffer [0]));
    switch (type)
    {
        case rai::block_type::send:
        {
            boost::asio::async_read (connection->connection->socket, boost::asio::buffer (receive_buffer.data () + 1, rai::send_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
            {
                this_l->received_block (ec, size_a);
            });
            break;
        }
        case rai::block_type::receive:
        {
            boost::asio::async_read (connection->connection->socket, boost::asio::buffer (receive_buffer.data () + 1, rai::receive_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
            {
                this_l->received_block (ec, size_a);
            });
            break;
        }
        case rai::block_type::open:
        {
            boost::asio::async_read (connection->connection->socket, boost::asio::buffer (receive_buffer.data () + 1, rai::open_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
            {
                this_l->received_block (ec, size_a);
            });
            break;
        }
        case rai::block_type::change:
        {
            boost::asio::async_read (connection->connection->socket, boost::asio::buffer (receive_buffer.data () + 1, rai::change_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
            {
                this_l->received_block (ec, size_a);
            });
            break;
        }
        case rai::block_type::not_a_block:
        {
            request ();
            break;
        }
        default:
        {
            BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Unknown type received as block type: %1%") % static_cast <int> (type));
            break;
        }
    }
}

rai::block_hash rai::bulk_pull_client::first ()
{
	rai::block_hash result (0);
	rai::transaction transaction (connection->connection->node->store.environment, nullptr, false);
	auto iterator (connection->connection->node->store.unchecked_begin (transaction));
	if (iterator != connection->connection->node->store.unchecked_end ())
	{
		result = rai::block_hash (iterator->first);
	}
	else
	{
		if (connection->connection->node->config.logging.bulk_pull_logging ())
		{
			BOOST_LOG (connection->connection->node->log) << "Nothing left in unchecked table";
		}
	}
	return result;
}

void rai::bulk_pull_client::process_end ()
{
	block_flush ();
	rai::pull_synchronization synchronization (connection->connection->node->log, [this] (rai::transaction & transaction_a, rai::block const & block_a)
	{
		connection->connection->node->process_receive_many (transaction_a, block_a, [this] (rai::process_return result_a, rai::block const & block_a)
		{
			switch (result_a.code)
			{
				case rai::process_result::progress:
				case rai::process_result::old:
					break;
				case rai::process_result::fork:
					connection->connection->node->network.broadcast_confirm_req (block_a);
					BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Fork received in bootstrap for block: %1%") % block_a.hash ().to_string ());
					break;
				case rai::process_result::gap_previous:
				case rai::process_result::gap_source:
					if (connection->connection->node->config.logging.bulk_pull_logging ())
					{
						// Any activity while bootstrapping can cause gaps so these aren't as noteworthy
						BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Gap received in bootstrap for block: %1%") % block_a.hash ().to_string ());
					}
					break;
				default:
					BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Error inserting block in bootstrap: %1%") % block_a.hash ().to_string ());
					break;
			}
		});
		connection->connection->node->store.unchecked_del (transaction_a, block_a.hash ());
	}, connection->connection->node->store);
	rai::block_hash block (first ());
    while (!block.is_zero ())
    {
		{
			rai::transaction transaction (connection->connection->node->store.environment, nullptr, true);
			BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Commiting block: %1% and dependencies") % block.to_string ());
			auto error (synchronization.synchronize (transaction, block));
			if (error)
			{
				while (!synchronization.blocks.empty ())
				{
					std::unique_ptr <rai::block> block;
					auto hash (synchronization.blocks.top ());
					synchronization.blocks.pop ();
					if (!connection->connection->node->store.block_exists (transaction, hash))
					{
						if (connection->connection->node->config.logging.bulk_pull_logging ())
						{
							BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Dumping: %1%") % hash.to_string ());
						}
					}
					else
					{
						if (connection->connection->node->config.logging.bulk_pull_logging ())
						{
							BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Forcing: %1%") % hash.to_string ());
						}
						auto block (connection->connection->node->store.unchecked_get (transaction, hash));
					}
					connection->connection->node->store.unchecked_del (transaction, hash);
					if (block != nullptr)
					{
						connection->connection->node->process_receive_many (transaction, *block);
					}
				}
			}
		}
		block = first ();
    }
	connection->connection->node->wallets.search_pending_all ();
}

void rai::bulk_pull_client::block_flush ()
{
	rai::transaction transaction (connection->connection->node->store.environment, nullptr, true);
	for (auto & i: blocks)
	{
		connection->connection->node->store.unchecked_put (transaction, i->hash(), *i);
	}
	blocks.clear ();
}

void rai::bulk_pull_client::received_block (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		rai::bufferstream stream (receive_buffer.data (), 1 + size_a);
		auto block (rai::deserialize_block (stream));
		if (block != nullptr)
		{
            auto hash (block->hash ());
            if (connection->connection->node->config.logging.bulk_pull_logging ())
            {
                std::string block_l;
                block->serialize_json (block_l);
                BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Pulled block %1% %2%") % hash.to_string () % block_l);
            }
			blocks.emplace_back (std::move (block));
			if (blocks.size () == block_count)
			{
				block_flush ();
			}
            receive_block ();
		}
        else
        {
            BOOST_LOG (connection->connection->node->log) << "Error deserializing block received from pull request";
        }
	}
	else
	{
		BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Error bulk receiving block: %1%") % ec.message ());
	}
}

rai::bulk_pull_client::bulk_pull_client (std::shared_ptr <rai::frontier_req_client> const & connection_a) :
connection (connection_a),
current (connection->pulls.begin ()),
end (connection->pulls.end ())
{
	blocks.reserve (block_count);
}

rai::bulk_pull_client::~bulk_pull_client ()
{
    if (connection->connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->connection->node->log) << "Exiting bulk pull client";
    }
}

rai::bulk_push_client::bulk_push_client (std::shared_ptr <rai::frontier_req_client> const & connection_a) :
connection (connection_a),
synchronization (connection->connection->node->log, [this] (rai::transaction & transaction_a, rai::block const & block_a)
{
    push_block (block_a);
}, connection_a->connection->node->store)
{
}

rai::bulk_push_client::~bulk_push_client ()
{
    if (connection->connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->connection->node->log) << "Exiting bulk push client";
    }
}

void rai::bulk_push_client::start ()
{
    rai::bulk_push message;
    auto buffer (std::make_shared <std::vector <uint8_t>> ());
    {
        rai::vectorstream stream (*buffer);
        message.serialize (stream);
    }
    auto this_l (shared_from_this ());
    boost::asio::async_write (connection->connection->socket, boost::asio::buffer (buffer->data (), buffer->size ()), [this_l, buffer] (boost::system::error_code const & ec, size_t size_a)
        {
            if (!ec)
            {
                this_l->push ();
            }
            else
            {
                BOOST_LOG (this_l->connection->connection->node->log) << boost::str (boost::format ("Unable to send bulk_push request %1%") % ec.message ());
            }
        });
}

void rai::bulk_push_client::push ()
{
	auto finished (false);
	{
		rai::transaction transaction (connection->connection->node->store.environment, nullptr, true);
		auto first (connection->connection->node->store.unsynced_begin (transaction));
		if (first != rai::store_iterator (nullptr))
		{
			rai::block_hash hash (first->first);
			if (!hash.is_zero ())
			{
				connection->connection->node->store.unsynced_del (transaction, hash);
				synchronization.blocks.push (hash);
				synchronization.synchronize_one (transaction);
			}
			else
			{
				finished = true;
			}
		}
		else
		{
			finished = true;
		}
	}
	if (finished)
    {
        send_finished ();
    }
}

void rai::bulk_push_client::send_finished ()
{
    auto buffer (std::make_shared <std::vector <uint8_t>> ());
    buffer->push_back (static_cast <uint8_t> (rai::block_type::not_a_block));
    if (connection->connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->connection->node->log) << "Bulk push finished";
    }
    auto this_l (shared_from_this ());
    async_write (connection->connection->socket, boost::asio::buffer (buffer->data (), 1), [this_l] (boost::system::error_code const & ec, size_t size_a)
        {
            this_l->connection->completed_pushes ();
        });
}

void rai::bulk_push_client::push_block (rai::block const & block_a)
{
    auto buffer (std::make_shared <std::vector <uint8_t>> ());
    {
        rai::vectorstream stream (*buffer);
        rai::serialize_block (stream, block_a);
    }
    auto this_l (shared_from_this ());
    boost::asio::async_write (connection->connection->socket, boost::asio::buffer (buffer->data (), buffer->size ()), [this_l, buffer] (boost::system::error_code const & ec, size_t size_a)
	{
		if (!ec)
		{
			if (!this_l->synchronization.blocks.empty ())
			{
				rai::transaction transaction (this_l->connection->connection->node->store.environment, nullptr, true);
				this_l->synchronization.synchronize_one (transaction);
			}
			else
			{
				this_l->push ();
			}
		}
		else
		{
			BOOST_LOG (this_l->connection->connection->node->log) << boost::str (boost::format ("Error sending block during bulk push %1%") % ec.message ());
		}
	});
}

rai::bootstrap_attempt::bootstrap_attempt (std::shared_ptr <rai::node> node_a, std::vector <rai::endpoint> const & peers_a) :
node (node_a),
peers (peers_a),
connected (false)
{
}

rai::bootstrap_attempt::~bootstrap_attempt ()
{
	std::lock_guard <std::mutex> lock (node->bootstrap_initiator.mutex);
	node->bootstrap_initiator.notify_listeners ();
}

void rai::bootstrap_attempt::attempt ()
{
	assert (!node->bootstrap_initiator.mutex.try_lock ());
	if (!peers.empty () && !connected)
	{
		rai::endpoint endpoint (peers.back ());
		peers.pop_back ();
		BOOST_LOG (node->log) << boost::str (boost::format ("Initiating bootstrap to: %1%") % endpoint);
		auto node_l (node->shared ());
		auto this_l (shared_from_this ());
		auto processor (std::make_shared <rai::bootstrap_client> (node_l, this_l));
		attempts.push_back (processor);
		processor->run (rai::tcp_endpoint (endpoint.address (), endpoint.port ()));
		node->alarm.add (std::chrono::system_clock::now () + std::chrono::milliseconds (250), [this_l] ()
		{
			std::lock_guard <std::mutex> lock (this_l->node->bootstrap_initiator.mutex);
			this_l->attempt ();
		});
	}
}

void rai::bootstrap_attempt::stop ()
{
	for (auto i: attempts)
	{
		auto attempt (i.lock ());
		if (attempt != nullptr)
		{
			attempt->socket.close ();
		}
	}
}

rai::bootstrap_initiator::bootstrap_initiator (rai::node & node_a) :
node (node_a),
warmed_up (false)
{
}

void rai::bootstrap_initiator::warmup (rai::endpoint const & endpoint_a)
{
	auto do_warmup (false);
	{
		std::lock_guard <std::mutex> lock (mutex);
		auto attempt_l (attempt.lock ());
		if (attempt_l == nullptr)
		{
			if (warmed_up < 3)
			{
				++warmed_up;
				do_warmup = true;
			}
		}
		else
		{
			attempt_l->peers.push_back (endpoint_a);
			attempt_l->attempt ();
		}
	}
	if (do_warmup)
	{
		bootstrap_any ();
	}
}

void rai::bootstrap_initiator::bootstrap (rai::endpoint const & endpoint_a)
{
	std::vector <rai::endpoint> endpoints;
	endpoints.push_back (endpoint_a);
	begin_attempt (std::make_shared <rai::bootstrap_attempt> (node.shared (), endpoints));
}

void rai::bootstrap_initiator::bootstrap_any ()
{
	auto peers (node.peers.bootstrap_candidates ());
	std::vector <rai::endpoint> endpoints;
	for (auto &i: peers)
	{
		endpoints.push_back (i.endpoint);
	}
	std::random_shuffle (endpoints.begin (), endpoints.end ());
	begin_attempt (std::make_shared <bootstrap_attempt> (node.shared (), endpoints));
}

void rai::bootstrap_initiator::begin_attempt (std::shared_ptr <rai::bootstrap_attempt> attempt_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	if (!in_progress ())
	{
		attempt = attempt_a;
		attempt_a->attempt ();
		notify_listeners ();
	}
}

void rai::bootstrap_initiator::add_observer (std::function <void (bool)> const & observer_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	observers.push_back (observer_a);
}

bool rai::bootstrap_initiator::in_progress ()
{
	return attempt.lock () != nullptr;
}

void rai::bootstrap_initiator::stop ()
{
	auto attempt_l (attempt.lock ());
	if (attempt_l != nullptr)
	{
		attempt_l->stop ();
	}
}

void rai::bootstrap_initiator::notify_listeners ()
{
	assert (!mutex.try_lock());
	for (auto & i: observers)
	{
		i (in_progress ());
	}
}

rai::bootstrap_listener::bootstrap_listener (boost::asio::io_service & service_a, uint16_t port_a, rai::node & node_a) :
acceptor (service_a),
local (boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::any (), port_a)),
service (service_a),
node (node_a)
{
}

void rai::bootstrap_listener::start ()
{
    acceptor.open (local.protocol ());
    acceptor.set_option (boost::asio::ip::tcp::acceptor::reuse_address (true));
    acceptor.bind (local);
    acceptor.listen ();
    accept_connection ();
}

void rai::bootstrap_listener::stop ()
{
    on = false;
    acceptor.close ();
}

void rai::bootstrap_listener::accept_connection ()
{
    auto socket (std::make_shared <boost::asio::ip::tcp::socket> (service));
    acceptor.async_accept (*socket, [this, socket] (boost::system::error_code const & ec)
    {
        accept_action (ec, socket);
    });
}

void rai::bootstrap_listener::accept_action (boost::system::error_code const & ec, std::shared_ptr <boost::asio::ip::tcp::socket> socket_a)
{
    if (!ec)
    {
        accept_connection ();
        auto connection (std::make_shared <rai::bootstrap_server> (socket_a, node.shared ()));
        connection->receive ();
    }
    else
    {
        BOOST_LOG (node.log) << boost::str (boost::format ("Error while accepting bootstrap connections: %1%") % ec.message ());
    }
}

boost::asio::ip::tcp::endpoint rai::bootstrap_listener::endpoint ()
{
    return boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::loopback (), local.port ());
}

rai::bootstrap_server::~bootstrap_server ()
{
    if (node->config.logging.network_logging ())
    {
        BOOST_LOG (node->log) << "Exiting bootstrap server";
    }
}

rai::bootstrap_server::bootstrap_server (std::shared_ptr <boost::asio::ip::tcp::socket> socket_a, std::shared_ptr <rai::node> node_a) :
socket (socket_a),
node (node_a)
{
}

void rai::bootstrap_server::receive ()
{
    auto this_l (shared_from_this ());
    boost::asio::async_read (*socket, boost::asio::buffer (receive_buffer.data (), 8), [this_l] (boost::system::error_code const & ec, size_t size_a)
    {
        this_l->receive_header_action (ec, size_a);
    });
}

void rai::bootstrap_server::receive_header_action (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        assert (size_a == 8);
		rai::bufferstream type_stream (receive_buffer.data (), size_a);
		uint8_t version_max;
		uint8_t version_using;
		uint8_t version_min;
		rai::message_type type;
		std::bitset <16> extensions;
		if (!rai::message::read_header (type_stream, version_max, version_using, version_min, type, extensions))
		{
			switch (type)
			{
				case rai::message_type::bulk_pull:
				{
					auto this_l (shared_from_this ());
					boost::asio::async_read (*socket, boost::asio::buffer (receive_buffer.data () + 8, sizeof (rai::uint256_union) + sizeof (rai::uint256_union)), [this_l] (boost::system::error_code const & ec, size_t size_a)
					{
						this_l->receive_bulk_pull_action (ec, size_a);
					});
					break;
				}
				case rai::message_type::frontier_req:
				{
					auto this_l (shared_from_this ());
					boost::asio::async_read (*socket, boost::asio::buffer (receive_buffer.data () + 8, sizeof (rai::uint256_union) + sizeof (uint32_t) + sizeof (uint32_t)), [this_l] (boost::system::error_code const & ec, size_t size_a)
					{
						this_l->receive_frontier_req_action (ec, size_a);
					});
					break;
				}
                case rai::message_type::bulk_push:
                {
                    add_request (std::unique_ptr <rai::message> (new rai::bulk_push));
                    break;
                }
				default:
				{
					if (node->config.logging.network_logging ())
					{
						BOOST_LOG (node->log) << boost::str (boost::format ("Received invalid type from bootstrap connection %1%") % static_cast <uint8_t> (type));
					}
					break;
				}
			}
		}
    }
    else
    {
        if (node->config.logging.network_logging ())
        {
            BOOST_LOG (node->log) << boost::str (boost::format ("Error while receiving type %1%") % ec.message ());
        }
    }
}

void rai::bootstrap_server::receive_bulk_pull_action (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        std::unique_ptr <rai::bulk_pull> request (new rai::bulk_pull);
        rai::bufferstream stream (receive_buffer.data (), 8 + sizeof (rai::uint256_union) + sizeof (rai::uint256_union));
        auto error (request->deserialize (stream));
        if (!error)
        {
            if (node->config.logging.bulk_pull_logging ())
            {
                BOOST_LOG (node->log) << boost::str (boost::format ("Received bulk pull for %1% down to %2%") % request->start.to_string () % request->end.to_string ());
            }
			add_request (std::unique_ptr <rai::message> (request.release ()));
            receive ();
        }
    }
}

void rai::bootstrap_server::receive_frontier_req_action (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		std::unique_ptr <rai::frontier_req> request (new rai::frontier_req);
		rai::bufferstream stream (receive_buffer.data (), 8 + sizeof (rai::uint256_union) + sizeof (uint32_t) + sizeof (uint32_t));
		auto error (request->deserialize (stream));
		if (!error)
		{
			if (node->config.logging.bulk_pull_logging ())
			{
				BOOST_LOG (node->log) << boost::str (boost::format ("Received frontier request for %1% with age %2%") % request->start.to_string () % request->age);
			}
			add_request (std::unique_ptr <rai::message> (request.release ()));
			receive ();
		}
	}
    else
    {
        if (node->config.logging.network_logging ())
        {
            BOOST_LOG (node->log) << boost::str (boost::format ("Error sending receiving frontier request %1%") % ec.message ());
        }
    }
}

void rai::bootstrap_server::add_request (std::unique_ptr <rai::message> message_a)
{
	std::lock_guard <std::mutex> lock (mutex);
    auto start (requests.empty ());
	requests.push (std::move (message_a));
	if (start)
	{
		run_next ();
	}
}

void rai::bootstrap_server::finish_request ()
{
	std::lock_guard <std::mutex> lock (mutex);
	requests.pop ();
	if (!requests.empty ())
	{
		run_next ();
	}
}

namespace
{
class request_response_visitor : public rai::message_visitor
{
public:
    request_response_visitor (std::shared_ptr <rai::bootstrap_server> connection_a) :
    connection (connection_a)
    {
    }
    void keepalive (rai::keepalive const &) override
    {
        assert (false);
    }
    void publish (rai::publish const &) override
    {
        assert (false);
    }
    void confirm_req (rai::confirm_req const &) override
    {
        assert (false);
    }
    void confirm_ack (rai::confirm_ack const &) override
    {
        assert (false);
    }
    void bulk_pull (rai::bulk_pull const &) override
    {
        auto response (std::make_shared <rai::bulk_pull_server> (connection, std::unique_ptr <rai::bulk_pull> (static_cast <rai::bulk_pull *> (connection->requests.front ().release ()))));
        response->send_next ();
    }
    void bulk_push (rai::bulk_push const &) override
    {
        auto response (std::make_shared <rai::bulk_push_server> (connection));
        response->receive ();
    }
    void frontier_req (rai::frontier_req const &) override
    {
        auto response (std::make_shared <rai::frontier_req_server> (connection, std::unique_ptr <rai::frontier_req> (static_cast <rai::frontier_req *> (connection->requests.front ().release ()))));
        response->send_next ();
    }
    std::shared_ptr <rai::bootstrap_server> connection;
};
}

void rai::bootstrap_server::run_next ()
{
	assert (!requests.empty ());
    request_response_visitor visitor (shared_from_this ());
    requests.front ()->visit (visitor);
}

void rai::bulk_pull_server::set_current_end ()
{
    assert (request != nullptr);
	rai::transaction transaction (connection->node->store.environment, nullptr, false);
	if (!connection->node->store.block_exists (transaction, request->end))
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			BOOST_LOG (connection->node->log) << boost::str (boost::format ("Bulk pull end block doesn't exist: %1%, sending everything") % request->end.to_string ());
		}
		request->end.clear ();
	}
	rai::account_info info;
	auto no_address (connection->node->store.account_get (transaction, request->start, info));
	if (no_address)
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			BOOST_LOG (connection->node->log) << boost::str (boost::format ("Request for unknown account: %1%") % request->start.to_account ());
		}
		current = request->end;
	}
	else
	{
		if (!request->end.is_zero ())
		{
			auto account (connection->node->ledger.account (transaction, request->end));
			if (account == request->start)
			{
				current = info.head;
			}
			else
			{
				current = request->end;
			}
		}
		else
		{
			current = info.head;
		}
	}
}

void rai::bulk_pull_server::send_next ()
{
    std::unique_ptr <rai::block> block (get_next ());
    if (block != nullptr)
    {
        {
            send_buffer.clear ();
            rai::vectorstream stream (send_buffer);
            rai::serialize_block (stream, *block);
        }
        auto this_l (shared_from_this ());
        if (connection->node->config.logging.bulk_pull_logging ())
        {
            BOOST_LOG (connection->node->log) << boost::str (boost::format ("Sending block: %1%") % block->hash ().to_string ());
        }
        async_write (*connection->socket, boost::asio::buffer (send_buffer.data (), send_buffer.size ()), [this_l] (boost::system::error_code const & ec, size_t size_a)
        {
            this_l->sent_action (ec, size_a);
        });
    }
    else
    {
        send_finished ();
    }
}

std::unique_ptr <rai::block> rai::bulk_pull_server::get_next ()
{
    std::unique_ptr <rai::block> result;
    if (current != request->end)
    {
		rai::transaction transaction (connection->node->store.environment, nullptr, false);
        result = connection->node->store.block_get (transaction, current);
        assert (result != nullptr);
        auto previous (result->previous ());
        if (!previous.is_zero ())
        {
            current = previous;
        }
        else
        {
            request->end = current;
        }
    }
    return result;
}

void rai::bulk_pull_server::sent_action (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        send_next ();
    }
	else
	{
		BOOST_LOG (connection->node->log) << boost::str (boost::format ("Unable to bulk send block: %1%") % ec.message ());
	}
}

void rai::bulk_pull_server::send_finished ()
{
    send_buffer.clear ();
    send_buffer.push_back (static_cast <uint8_t> (rai::block_type::not_a_block));
    auto this_l (shared_from_this ());
    if (connection->node->config.logging.bulk_pull_logging ())
    {
        BOOST_LOG (connection->node->log) << "Bulk sending finished";
    }
    async_write (*connection->socket, boost::asio::buffer (send_buffer.data (), 1), [this_l] (boost::system::error_code const & ec, size_t size_a)
    {
        this_l->no_block_sent (ec, size_a);
    });
}

void rai::bulk_pull_server::no_block_sent (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        assert (size_a == 1);
		connection->finish_request ();
    }
	else
	{
		BOOST_LOG (connection->node->log) << "Unable to send not-a-block";
	}
}

rai::bulk_pull_server::bulk_pull_server (std::shared_ptr <rai::bootstrap_server> const & connection_a, std::unique_ptr <rai::bulk_pull> request_a) :
connection (connection_a),
request (std::move (request_a))
{
    set_current_end ();
}

rai::bulk_push_server::bulk_push_server (std::shared_ptr <rai::bootstrap_server> const & connection_a) :
connection (connection_a)
{
}

void rai::bulk_push_server::receive ()
{
    auto this_l (shared_from_this ());
    boost::asio::async_read (*connection->socket, boost::asio::buffer (receive_buffer.data (), 1), [this_l] (boost::system::error_code const & ec, size_t size_a)
        {
            if (!ec)
            {
                this_l->received_type ();
            }
            else
            {
                BOOST_LOG (this_l->connection->node->log) << boost::str (boost::format ("Error receiving block type %1%") % ec.message ());
            }
        });
}

void rai::bulk_push_server::received_type ()
{
    auto this_l (shared_from_this ());
    rai::block_type type (static_cast <rai::block_type> (receive_buffer [0]));
    switch (type)
    {
        case rai::block_type::send:
        {
            boost::asio::async_read (*connection->socket, boost::asio::buffer (receive_buffer.data () + 1, rai::send_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
                                     {
                                         this_l->received_block (ec, size_a);
                                     });
            break;
        }
        case rai::block_type::receive:
        {
            boost::asio::async_read (*connection->socket, boost::asio::buffer (receive_buffer.data () + 1, rai::receive_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
                                     {
                                         this_l->received_block (ec, size_a);
                                     });
            break;
        }
        case rai::block_type::open:
        {
            boost::asio::async_read (*connection->socket, boost::asio::buffer (receive_buffer.data () + 1, rai::open_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
                                     {
                                         this_l->received_block (ec, size_a);
                                     });
            break;
        }
        case rai::block_type::change:
        {
            boost::asio::async_read (*connection->socket, boost::asio::buffer (receive_buffer.data () + 1, rai::change_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
                                     {
                                         this_l->received_block (ec, size_a);
                                     });
            break;
        }
        case rai::block_type::not_a_block:
        {
            connection->finish_request ();
            break;
        }
        default:
        {
            BOOST_LOG (connection->node->log) << "Unknown type received as block type";
            break;
        }
    }
}

void rai::bulk_push_server::received_block (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        rai::bufferstream stream (receive_buffer.data (), 1 + size_a);
        auto block (rai::deserialize_block (stream));
        if (block != nullptr)
        {
            connection->node->process_receive_republish (std::move (block), 0);
            receive ();
        }
        else
        {
            BOOST_LOG (connection->node->log) << "Error deserializing block received from pull request";
        }
    }
}

rai::frontier_req_server::frontier_req_server (std::shared_ptr <rai::bootstrap_server> const & connection_a, std::unique_ptr <rai::frontier_req> request_a) :
connection (connection_a),
current (request_a->start.number () - 1),
info (0, 0, 0, 0, 0),
request (std::move (request_a))
{
	next ();
    skip_old ();
}

void rai::frontier_req_server::skip_old ()
{
    if (request->age != std::numeric_limits<decltype (request->age)>::max ())
    {
        auto now (connection->node->store.now ());
        while (!current.is_zero () && (now - info.modified) >= request->age)
        {
            next ();
        }
    }
}

void rai::frontier_req_server::send_next ()
{
    if (!current.is_zero ())
    {
        {
            send_buffer.clear ();
            rai::vectorstream stream (send_buffer);
            write (stream, current.bytes);
            write (stream, info.head.bytes);
        }
        auto this_l (shared_from_this ());
        if (connection->node->config.logging.bulk_pull_logging ())
        {
            BOOST_LOG (connection->node->log) << boost::str (boost::format ("Sending frontier for %1% %2%") % current.to_account () % info.head.to_string ());
        }
		next ();
        async_write (*connection->socket, boost::asio::buffer (send_buffer.data (), send_buffer.size ()), [this_l] (boost::system::error_code const & ec, size_t size_a)
        {
            this_l->sent_action (ec, size_a);
        });
    }
    else
    {
        send_finished ();
    }
}

void rai::frontier_req_server::send_finished ()
{
    {
        send_buffer.clear ();
        rai::vectorstream stream (send_buffer);
        rai::uint256_union zero (0);
        write (stream, zero.bytes);
        write (stream, zero.bytes);
    }
    auto this_l (shared_from_this ());
    if (connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->node->log) << "Frontier sending finished";
    }
    async_write (*connection->socket, boost::asio::buffer (send_buffer.data (), send_buffer.size ()), [this_l] (boost::system::error_code const & ec, size_t size_a)
    {
        this_l->no_block_sent (ec, size_a);
    });
}

void rai::frontier_req_server::no_block_sent (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
		connection->finish_request ();
    }
    else
    {
        if (connection->node->config.logging.network_logging ())
        {
            BOOST_LOG (connection->node->log) << boost::str (boost::format ("Error sending frontier finish %1%") % ec.message ());
        }
    }
}

void rai::frontier_req_server::sent_action (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        send_next ();
    }
    else
    {
        if (connection->node->config.logging.network_logging ())
        {
            BOOST_LOG (connection->node->log) << boost::str (boost::format ("Error sending frontier pair %1%") % ec.message ());
        }
    }
}

void rai::frontier_req_server::next ()
{
	rai::transaction transaction (connection->node->store.environment, nullptr, false);
	auto iterator (connection->node->store.latest_begin (transaction, current.number () + 1));
	if (iterator != connection->node->store.latest_end ())
	{
		current = rai::uint256_union (iterator->first);
		info = rai::account_info (iterator->second);
	}
	else
	{
		current.clear ();
	}
}
