/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "messaging_adapter.hpp"

#include "proton/sender.hpp"
#include "proton/error.hpp"
#include "proton/transport.hpp"

#include "contexts.hpp"
#include "messaging_event.hpp"
#include "container_impl.hpp"
#include "msg.hpp"

#include "proton/connection.h"
#include "proton/delivery.h"
#include "proton/handlers.h"
#include "proton/link.h"
#include "proton/message.h"
#include "proton/session.h"
#include "proton/transport.h"

namespace proton {

namespace {
void credit_topup(pn_link_t *link) {
    if (link && pn_link_is_receiver(link)) {
        int window = link_context::get(link).credit_window;
        if (window) {
            int delta = window - pn_link_credit(link);
            pn_link_flow(link, delta);
        }
    }
}
}

messaging_adapter::messaging_adapter(handler &delegate) : delegate_(delegate) {}

messaging_adapter::~messaging_adapter(){}

void messaging_adapter::on_reactor_init(proton_event &pe) {
    messaging_event mevent(messaging_event::START, pe);
    // Container specific event
    if (pe.container())
        delegate_.on_container_start(mevent, *pe.container());
}

void messaging_adapter::on_link_flow(proton_event &pe) {
    pn_event_t *pne = pe.pn_event();
    pn_link_t *lnk = pn_event_link(pne);
    sender s(lnk);
    if (lnk && pn_link_is_sender(lnk) && pn_link_credit(lnk) > 0) {
        // create on_message extended event
        messaging_event mevent(messaging_event::SENDABLE, pe);
        delegate_.on_sendable(mevent, s);
    }
    credit_topup(lnk);
}

void messaging_adapter::on_delivery(proton_event &pe) {
    pn_event_t *cevent = pe.pn_event();
    pn_link_t *lnk = pn_event_link(cevent);
    link_context& lctx = link_context::get(lnk);
    delivery dlv = pe.delivery();

    if (pn_link_is_receiver(lnk)) {
        if (!dlv.partial() && dlv.readable()) {
            // generate on_message
            messaging_event mevent(messaging_event::MESSAGE, pe);
            pn_connection_t *pnc = pn_session_connection(pn_link_session(lnk));
            connection_context& ctx = connection_context::get(pnc);
            // Reusable per-connection message.
            // Avoid expensive heap malloc/free overhead.
            // See PROTON-998
            class message &msg(ctx.event_message);
            mevent.message_ = &msg;
            mevent.message_->decode(dlv);
            if (pn_link_state(lnk) & PN_LOCAL_CLOSED) {
                if (lctx.auto_accept)
                    dlv.release();
            } else {
                delegate_.on_message(mevent, dlv, msg);
                if (lctx.auto_accept && !dlv.settled())
                    dlv.accept();
            }
        }
        else if (dlv.updated() && dlv.settled()) {
            messaging_event mevent(messaging_event::DELIVERY_SETTLE, pe);
            delegate_.on_delivery_settle(mevent, dlv);
        }
        credit_topup(lnk);
    } else {
        // sender
        if (dlv.updated()) {
            uint64_t rstate = dlv.remote_state();
            if (rstate == PN_ACCEPTED) {
                messaging_event mevent(messaging_event::DELIVERY_ACCEPT, pe);
                delegate_.on_delivery_accept(mevent, dlv);
            }
            else if (rstate == PN_REJECTED) {
                messaging_event mevent(messaging_event::DELIVERY_REJECT, pe);
                delegate_.on_delivery_reject(mevent, dlv);
            }
            else if (rstate == PN_RELEASED || rstate == PN_MODIFIED) {
                messaging_event mevent(messaging_event::DELIVERY_RELEASE, pe);
                delegate_.on_delivery_release(mevent, dlv);
            }

            if (dlv.settled()) {
                messaging_event mevent(messaging_event::DELIVERY_SETTLE, pe);
                delegate_.on_delivery_settle(mevent, dlv);
            }
            if (lctx.auto_settle)
                dlv.settle();
        }
    }
}

namespace {

bool is_local_open(pn_state_t state) {
    return state & PN_LOCAL_ACTIVE;
}

bool is_local_unititialised(pn_state_t state) {
    return state & PN_LOCAL_UNINIT;
}

} // namespace

void messaging_adapter::on_link_remote_close(proton_event &pe) {
    pn_event_t *cevent = pe.pn_event();
    pn_link_t *lnk = pn_event_link(cevent);
    messaging_event clevent(messaging_event::LINK_CLOSE, pe);
    messaging_event eevent(messaging_event::LINK_ERROR, pe);
    if (pn_link_is_receiver(lnk)) {
        receiver r = link(lnk).receiver();
        if (pn_condition_is_set(pn_link_remote_condition(lnk))) {
            delegate_.on_receiver_error(eevent, r);
        }
        delegate_.on_receiver_close(clevent, r);
    } else {
        sender s = link(lnk).sender();
        if (pn_condition_is_set(pn_link_remote_condition(lnk))) {
            delegate_.on_sender_error(eevent, s);
        }
        delegate_.on_sender_close(clevent, s);
    }
    pn_link_close(lnk);
}

void messaging_adapter::on_session_remote_close(proton_event &pe) {
    pn_event_t *cevent = pe.pn_event();
    pn_session_t *session = pn_event_session(cevent);
    class session s(session);
    if (pn_condition_is_set(pn_session_remote_condition(session))) {
        messaging_event mevent(messaging_event::SESSION_ERROR, pe);
        delegate_.on_session_error(mevent, s);
    }
    messaging_event mevent(messaging_event::SESSION_CLOSE, pe);
    delegate_.on_session_close(mevent, s);
    pn_session_close(session);
}

void messaging_adapter::on_connection_remote_close(proton_event &pe) {
    pn_event_t *cevent = pe.pn_event();
    pn_connection_t *connection = pn_event_connection(cevent);
    class connection c(connection);
    if (pn_condition_is_set(pn_connection_remote_condition(connection))) {
        messaging_event mevent(messaging_event::CONNECTION_ERROR, pe);
        delegate_.on_connection_error(mevent, c);
    }
    messaging_event mevent(messaging_event::CONNECTION_CLOSE, pe);
    delegate_.on_connection_close(mevent, c);
    pn_connection_close(connection);
}

void messaging_adapter::on_connection_remote_open(proton_event &pe) {
    messaging_event mevent(messaging_event::CONNECTION_OPEN, pe);
    pn_connection_t *connection = pn_event_connection(pe.pn_event());
    class connection c(connection);
    delegate_.on_connection_open(mevent, c);
    if (!is_local_open(pn_connection_state(connection)) && is_local_unititialised(pn_connection_state(connection))) {
        pn_connection_open(connection);
    }
}

void messaging_adapter::on_session_remote_open(proton_event &pe) {
    messaging_event mevent(messaging_event::SESSION_OPEN, pe);
    pn_session_t *session = pn_event_session(pe.pn_event());
    class session s(session);
    delegate_.on_session_open(mevent, s);
    if (!is_local_open(pn_session_state(session)) && is_local_unititialised(pn_session_state(session))) {
        pn_session_open(session);
    }
}

void messaging_adapter::on_link_local_open(proton_event &pe) {
    credit_topup(pn_event_link(pe.pn_event()));
}

void messaging_adapter::on_link_remote_open(proton_event &pe) {
    messaging_event mevent(messaging_event::LINK_OPEN, pe);
    pn_link_t *lnk = pn_event_link(pe.pn_event());
    if (pn_link_is_receiver(lnk)) {
      receiver r = link(lnk).receiver();
      delegate_.on_receiver_open(mevent, r);
    } else {
      sender s = link(lnk).sender();
      delegate_.on_sender_open(mevent, s);
    }
    if (!is_local_open(pn_link_state(lnk)) && is_local_unititialised(pn_link_state(lnk))) {
        link l(lnk);
        if (pe.container())
            l.open(pe.container()->impl_->link_options_);
        else
            l.open();    // No default for engine
    }
    credit_topup(lnk);
}

void messaging_adapter::on_transport_tail_closed(proton_event &pe) {
    pn_connection_t *conn = pn_event_connection(pe.pn_event());
    if (conn && is_local_open(pn_connection_state(conn))) {
        pn_transport_t *tspt = pn_event_transport(pe.pn_event());
        transport t(tspt);
        if (pn_condition_is_set(pn_transport_condition(tspt))) {
            messaging_event mevent(messaging_event::TRANSPORT_ERROR, pe);
            delegate_.on_transport_error(mevent, t);
        }
        messaging_event mevent(messaging_event::TRANSPORT_CLOSE, pe);
        delegate_.on_transport_close(mevent, t);
    }
}

void messaging_adapter::on_timer_task(proton_event& pe)
{
    if (pe.container()) {
        messaging_event mevent(messaging_event::TIMER, pe);
        delegate_.on_timer(mevent, *pe.container());
    }
}

}
