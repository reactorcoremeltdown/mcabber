/*
 * jabglue.c    -- Jabber protocol handling
 *
 * Copyright (C) 2005, 2006 Mikael Berthe <bmikael@lists.lilotux.net>
 * Parts come from the centericq project:
 * Copyright (C) 2002-2005 by Konstantin Klyagin <konst@konst.org.ua>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include "../libjabber/jabber.h"
#include "jabglue.h"
#include "jab_priv.h"
#include "roster.h"
#include "screen.h"
#include "hooks.h"
#include "utils.h"
#include "settings.h"
#include "hbuf.h"
#include "histolog.h"
#include "commands.h"

#define JABBERPORT      5222
#define JABBERSSLPORT   5223

#define RECONNECTION_TIMEOUT    60L

jconn jc;
guint AutoConnection;
enum enum_jstate jstate;

char imstatus2char[imstatus_size+1] = {
    '_', 'o', 'i', 'f', 'd', 'n', 'a', '\0'
};

static time_t LastPingTime;
static unsigned int KeepaliveDelay;
static enum imstatus mystatus = offline;
static enum imstatus mywantedstatus = available;
static gchar *mystatusmsg;
static unsigned char online;

static void statehandler(jconn, int);
static void packethandler(jconn, jpacket);
void handle_state_events(char* from, xmlnode xmldata);

static void logger(jconn j, int io, const char *buf)
{
  scr_LogPrint(LPRINT_DEBUG, "%03s: %s", ((io == 0) ? "OUT" : "IN"), buf);
}

//  jidtodisp(jid)
// Strips the resource part from the jid
// The caller should g_free the result after use.
char *jidtodisp(const char *jid)
{
  char *ptr;
  char *alias;

  alias = g_strdup(jid);

  if ((ptr = strchr(alias, JID_RESOURCE_SEPARATOR)) != NULL) {
    *ptr = 0;
  }
  return alias;
}

char *compose_jid(const char *username, const char *servername,
                  const char *resource)
{
  char *jid = g_new(char, 3 +
                    strlen(username) + strlen(servername) + strlen(resource));
  strcpy(jid, username);
  if (!strchr(jid, JID_DOMAIN_SEPARATOR)) {
    strcat(jid, JID_DOMAIN_SEPARATORSTR);
    strcat(jid, servername);
  }
  strcat(jid, JID_RESOURCE_SEPARATORSTR);
  strcat(jid, resource);
  return jid;
}

inline unsigned char jb_getonline(void)
{
  return online;
}

jconn jb_connect(const char *jid, const char *server, unsigned int port,
                 int ssl, const char *pass)
{
  if (!port) {
    if (ssl)
      port = JABBERSSLPORT;
    else
      port = JABBERPORT;
  }

  jb_disconnect();

  if (!jid) return jc;

  jc = jab_new((char*)jid, (char*)pass, (char*)server, port, ssl);

  /* These 3 functions can deal with a NULL jc, no worry... */
  jab_logger(jc, logger);
  jab_packet_handler(jc, &packethandler);
  jab_state_handler(jc, &statehandler);

  if (jc && jc->user) {
    online = TRUE;
    jstate = STATE_CONNECTING;
    statehandler(0, -1);
    jab_start(jc);
  }

  return jc;
}

void jb_disconnect(void)
{
  if (!jc) return;

  if (online) {
    // Announce it to  everyone else
    jb_setstatus(offline, NULL, "");
    // End the XML flow
    jb_send_raw("</stream:stream>");
    /*
    // Free status message
    g_free(mystatusmsg);
    mystatusmsg = NULL;
    */
  }

  // Announce it to the user
  statehandler(jc, JCONN_STATE_OFF);

  jab_delete(jc);
  jc = NULL;
}

inline void jb_reset_keepalive()
{
  time(&LastPingTime);
}

void jb_send_raw(const char *str)
{
  if (jc && online && str)
    jab_send_raw(jc, str);
}

void jb_keepalive()
{
  if (jc && online)
    jab_send_raw(jc, "  \t  ");
  jb_reset_keepalive();
}

void jb_set_keepalive_delay(unsigned int delay)
{
  KeepaliveDelay = delay;
}

//  check_connection()
// Check if we've been disconnected for a while (predefined timeout),
// and if so try to reconnect.
static void check_connection(void)
{
  static time_t disconnection_timestamp = 0L;
  time_t now;

  // Maybe we're voluntarily offline...
  if (!AutoConnection)
    return;

  // Are we totally disconnected?
  if (jc && jc->state != JCONN_STATE_OFF) {
    disconnection_timestamp = 0L;
    return;
  }

  time(&now);
  if (!disconnection_timestamp) {
    disconnection_timestamp = now;
    return;
  }

  // If the reconnection_timeout is reached, try to reconnect.
  if (now > disconnection_timestamp + RECONNECTION_TIMEOUT) {
    mcabber_connect();
    disconnection_timestamp = 0L;
  }
}

void jb_main()
{
  time_t now;
  fd_set fds;
  long timeout;
  struct timeval tv;
  static time_t last_eviqs_check = 0L;

  if (!online) {
    safe_usleep(10000);
    check_connection();
    return;
  }

  if (jc && jc->state == JCONN_STATE_CONNECTING) {
    safe_usleep(75000);
    jab_start(jc);
    return;
  }

  FD_ZERO(&fds);
  FD_SET(0, &fds);
  FD_SET(jc->fd, &fds);

  tv.tv_sec = 60;
  tv.tv_usec = 0;

  time(&now);

  if (KeepaliveDelay) {
    if (now >= LastPingTime + (time_t)KeepaliveDelay) {
      tv.tv_sec = 0;
    } else {
      tv.tv_sec = LastPingTime + (time_t)KeepaliveDelay - now;
    }
  }

  // Check auto-away timeout
  timeout = scr_GetAutoAwayTimeout(now);
  if (tv.tv_sec > timeout) {
    tv.tv_sec = timeout;
  }

#if defined JEP0022 || defined JEP0085
  // Check composing timeout
  timeout = scr_GetChatStatesTimeout(now);
  if (tv.tv_sec > timeout) {
    tv.tv_sec = timeout;
  }
#endif

  if (!tv.tv_sec)
    tv.tv_usec = 350000;

  scr_DoUpdate();
  if (select(jc->fd + 1, &fds, NULL, NULL, &tv) > 0) {
    if (FD_ISSET(jc->fd, &fds))
      jab_poll(jc, 0);
  }

  if (jstate == STATE_CONNECTING) {
    if (jc) {
      eviqs *iqn;
      xmlnode z;

      iqn = iqs_new(JPACKET__GET, NS_AUTH, "auth", IQS_DEFAULT_TIMEOUT);
      iqn->callback = &iqscallback_auth;

      z = xmlnode_insert_tag(xmlnode_get_tag(iqn->xmldata, "query"),
                             "username");
      xmlnode_insert_cdata(z, jc->user->user, (unsigned) -1);
      jab_send(jc, iqn->xmldata);

      jstate = STATE_GETAUTH;
    }

    if (!jc || jc->state == JCONN_STATE_OFF) {
      scr_LogPrint(LPRINT_LOGNORM, "Unable to connect to the server");
      online = FALSE;
    }
  }

  if (!jc) {
    statehandler(jc, JCONN_STATE_OFF);
  } else if (jc->state == JCONN_STATE_OFF || jc->fd == -1) {
    statehandler(jc, JCONN_STATE_OFF);
  }

  time(&now);

  // Check for EV & IQ requests timeouts
  if (now > last_eviqs_check + 20) {
    iqs_check_timeout(now);
    evs_check_timeout(now);
    last_eviqs_check = now;
  }

  // Keepalive
  if (KeepaliveDelay) {
    if (now > LastPingTime + (time_t)KeepaliveDelay)
      jb_keepalive();
  }
}

inline enum imstatus jb_getstatus()
{
  return mystatus;
}

inline const char *jb_getstatusmsg()
{
  return mystatusmsg;
}

static void roompresence(gpointer room, void *presencedata)
{
  const char *jid;
  const char *nickname;
  char *to;
  struct T_presence *pres = presencedata;

  if (!buddy_getinsideroom(room))
    return;

  jid = buddy_getjid(room);
  if (!jid) return;
  nickname = buddy_getnickname(room);
  if (!nickname) return;

  to = g_strdup_printf("%s/%s", jid, nickname);
  jb_setstatus(pres->st, to, pres->msg);
  g_free(to);
}

//  presnew(status, recipient, message)
// Create an xmlnode with default presence attributes
// Note: the caller must free the node after use
static xmlnode presnew(enum imstatus st, const char *recipient,
                       const char *msg)
{
  unsigned int prio;
  xmlnode x;

  x = jutil_presnew(JPACKET__UNKNOWN, 0, 0);

  if (recipient) {
    xmlnode_put_attrib(x, "to", recipient);
  }

  switch(st) {
    case away:
        xmlnode_insert_cdata(xmlnode_insert_tag(x, "show"), "away",
                             (unsigned) -1);
        break;

    case dontdisturb:
        xmlnode_insert_cdata(xmlnode_insert_tag(x, "show"), "dnd",
                             (unsigned) -1);
        break;

    case freeforchat:
        xmlnode_insert_cdata(xmlnode_insert_tag(x, "show"), "chat",
                             (unsigned) -1);
        break;

    case notavail:
        xmlnode_insert_cdata(xmlnode_insert_tag(x, "show"), "xa",
                             (unsigned) -1);
        break;

    case invisible:
        xmlnode_put_attrib(x, "type", "invisible");
        break;

    case offline:
        xmlnode_put_attrib(x, "type", "unavailable");
        break;

    default:
        break;
  }

  prio = settings_opt_get_int("priority");
  if (prio) {
    char strprio[8];
    snprintf(strprio, 8, "%d", (int)prio);
    xmlnode_insert_cdata(xmlnode_insert_tag(x, "priority"),
                         strprio, (unsigned) -1);
  }

  if (msg)
    xmlnode_insert_cdata(xmlnode_insert_tag(x, "status"), msg, (unsigned) -1);

  return x;
}

void jb_setstatus(enum imstatus st, const char *recipient, const char *msg)
{
  xmlnode x;

  if (msg) {
    // The status message has been specified.  We'll use it, unless it is
    // "-" which is a special case (option meaning "no status message").
    if (!strcmp(msg, "-"))
      msg = "";
  } else {
    // No status message specified; we'll use:
    // a) the default status message (if provided by the user);
    // b) the current status message;
    // c) no status message (i.e. an empty one).
    msg = settings_get_status_msg(st);
    if (!msg) {
      if (mystatusmsg)
        msg = mystatusmsg;
      else
        msg = "";
    }
  }

  // Only send the packet if we're online.
  // (But we want to update internal status even when disconnected,
  // in order to avoid some problems during network failures)
  if (online) {
    x = presnew(st, recipient, (st != invisible ? msg : NULL));
    jab_send(jc, x);
    xmlnode_free(x);
  }

  // If we didn't change our _global_ status, we are done
  if (recipient) return;

  if (online) {
    // Send presence to chatrooms
    if (st != invisible) {
      struct T_presence room_presence;
      room_presence.st = st;
      room_presence.msg = msg;
      foreach_buddy(ROSTER_TYPE_ROOM, &roompresence, &room_presence);
    }
  }

  if (online) {
    // We'll need to update the roster if we switch to/from offline because
    // we don't know the presences of buddies when offline...
    if (mystatus == offline || st == offline)
      update_roster = TRUE;

    hk_mystatuschange(0, mystatus, st, (st != invisible ? msg : ""));
    mystatus = st;
  }
  if (st)
    mywantedstatus = st;
  if (msg != mystatusmsg) {
    g_free(mystatusmsg);
    if (*msg)
      mystatusmsg = g_strdup(msg);
    else
      mystatusmsg = NULL;
  }

  // Update status line
  scr_UpdateMainStatus(TRUE);
}

//  jb_setprevstatus()
// Set previous status.  This wrapper function is used after a disconnection.
inline void jb_setprevstatus(void)
{
  jb_setstatus(mywantedstatus, NULL, mystatusmsg);
}

//  new_msgid()
// Generate a new id string.  The caller should free it.
static char *new_msgid(void)
{
  static guint msg_idn;
  time_t now;
  time(&now);
  if (!msg_idn)
    srand(now);
  msg_idn += 1U + (unsigned int) (9.0 * (rand() / (RAND_MAX + 1.0)));
  return g_strdup_printf("%u%d", msg_idn, (int)(now%10L));
}

void jb_send_msg(const char *jid, const char *text, int type,
                 const char *subject, const char *msgid)
{
  xmlnode x;
  gchar *strtype;
#if defined JEP0022 || defined JEP0085
  xmlnode event;
  char *rname, *barejid;
  GSList *sl_buddy;
  guint use_jep85 = 0;
  struct jep0085 *jep85 = NULL;
#endif

  if (!online) return;

  if (type == ROSTER_TYPE_ROOM)
    strtype = TMSG_GROUPCHAT;
  else
    strtype = TMSG_CHAT;

  x = jutil_msgnew(strtype, (char*)jid, NULL, (char*)text);
  if (subject) {
    xmlnode y;
    y = xmlnode_insert_tag(x, "subject");
    xmlnode_insert_cdata(y, subject, (unsigned) -1);
  }

#if defined JEP0022 || defined JEP0085
  // If typing notifications are disabled, we can skip all this stuff...
  if (chatstates_disabled || type == ROSTER_TYPE_ROOM)
    goto jb_send_msg_no_chatstates;

  rname = strchr(jid, JID_RESOURCE_SEPARATOR);
  barejid = jidtodisp(jid);
  sl_buddy = roster_find(barejid, jidsearch, ROSTER_TYPE_USER);
  g_free(barejid);

  // If we can get a resource name, we use it.  Else we use NULL,
  // which hopefully will give us the most likely resource.
  if (rname)
    rname++;
  if (sl_buddy)
    jep85 = buddy_resource_jep85(sl_buddy->data, rname);
#endif

#ifdef JEP0085
  /* JEP-0085 5.1
   * "Until receiving a reply to the initial content message (or a standalone
   * notification) from the Contact, the User MUST NOT send subsequent chat
   * state notifications to the Contact."
   * In our implementation support is initially "unknown", they it's "probed"
   * and can become "ok".
   */
  if (jep85 && (jep85->support == CHATSTATES_SUPPORT_OK ||
                jep85->support == CHATSTATES_SUPPORT_UNKNOWN)) {
    event = xmlnode_insert_tag(x, "active");
    xmlnode_put_attrib(event, "xmlns", NS_CHATSTATES);
    if (jep85->support == CHATSTATES_SUPPORT_UNKNOWN)
      jep85->support = CHATSTATES_SUPPORT_PROBED;
    else
      use_jep85 = 1;
    jep85->last_state_sent = ROSTER_EVENT_ACTIVE;
  }
#endif
#ifdef JEP0022
  /* JEP-22
   * If the Contact supports JEP-0085, we do not use JEP-0022.
   * If not, we try to fall back to JEP-0022.
   */
  if (!use_jep85) {
    struct jep0022 *jep22 = NULL;
    event = xmlnode_insert_tag(x, "x");
    xmlnode_put_attrib(event, "xmlns", NS_EVENT);
    xmlnode_insert_tag(event, "composing");

    if (sl_buddy)
      jep22 = buddy_resource_jep22(sl_buddy->data, rname);
    if (jep22)
      jep22->last_state_sent = ROSTER_EVENT_ACTIVE;

    // An id is mandatory when using JEP-0022.
    if (!msgid && (text || subject)) {
      msgid = new_msgid();
      // Let's update last_msgid_sent
      // (We do not update it when the msgid is provided by the caller,
      // because this is probably a special message...)
      if (jep22) {
        g_free(jep22->last_msgid_sent);
        jep22->last_msgid_sent = g_strdup(msgid);
      }
    }
  }
#endif

jb_send_msg_no_chatstates:
  xmlnode_put_attrib(x, "id", msgid);

  jab_send(jc, x);
  xmlnode_free(x);

  jb_reset_keepalive();
}


#ifdef JEP0085
//  jb_send_jep85_chatstate()
// Send a JEP-85 chatstate.
static void jb_send_jep85_chatstate(const char *jid, guint state)
{
  xmlnode x;
  xmlnode event;
  char *rname, *barejid;
  GSList *sl_buddy;
  const char *chattag;
  struct jep0085 *jep85 = NULL;

  if (!online) return;

  rname = strchr(jid, JID_RESOURCE_SEPARATOR);
  barejid = jidtodisp(jid);
  sl_buddy = roster_find(barejid, jidsearch, ROSTER_TYPE_USER);
  g_free(barejid);

  // If we can get a resource name, we use it.  Else we use NULL,
  // which hopefully will give us the most likely resource.
  if (rname)
    rname++;
  if (sl_buddy)
    jep85 = buddy_resource_jep85(sl_buddy->data, rname);

  if (!jep85 || (jep85->support != CHATSTATES_SUPPORT_OK))
    return;

  if (state == jep85->last_state_sent)
    return;

  if (state == ROSTER_EVENT_ACTIVE)
    chattag = "active";
  else if (state == ROSTER_EVENT_COMPOSING)
    chattag = "composing";
  else if (state == ROSTER_EVENT_PAUSED)
    chattag = "paused";
  else {
    scr_LogPrint(LPRINT_LOGNORM, "Error: unsupported JEP-85 state (%d)", state);
    return;
  }

  jep85->last_state_sent = state;

  x = jutil_msgnew(TMSG_CHAT, (char*)jid, NULL, NULL);

  event = xmlnode_insert_tag(x, chattag);
  xmlnode_put_attrib(event, "xmlns", NS_CHATSTATES);

  jab_send(jc, x);
  xmlnode_free(x);

  jb_reset_keepalive();
}
#endif

#ifdef JEP0022
//  jb_send_jep22_event()
// Send a JEP-22 message event (delivered, composing...).
static void jb_send_jep22_event(const char *jid, guint type)
{
  xmlnode x;
  xmlnode event;
  const char *msgid;
  char *rname, *barejid;
  GSList *sl_buddy;
  struct jep0022 *jep22 = NULL;
  guint jep22_state;

  if (!online) return;

  rname = strchr(jid, JID_RESOURCE_SEPARATOR);
  barejid = jidtodisp(jid);
  sl_buddy = roster_find(barejid, jidsearch, ROSTER_TYPE_USER);
  g_free(barejid);

  // If we can get a resource name, we use it.  Else we use NULL,
  // which hopefully will give us the most likely resource.
  if (rname)
    rname++;
  if (sl_buddy)
    jep22 = buddy_resource_jep22(sl_buddy->data, rname);

  if (!jep22)
    return; // XXX Maybe we could try harder (other resources?)

  msgid = jep22->last_msgid_rcvd;

  // For composing events (composing, active, inactive, paused...),
  // JEP22 only has 2 states; we'll use composing and active.
  if (type == ROSTER_EVENT_COMPOSING)
    jep22_state = ROSTER_EVENT_COMPOSING;
  else if (type == ROSTER_EVENT_ACTIVE ||
           type == ROSTER_EVENT_PAUSED)
    jep22_state = ROSTER_EVENT_ACTIVE;
  else
    jep22_state = 0; // ROSTER_EVENT_NONE

  if (jep22_state) {
    // Do not re-send a same event
    if (jep22_state == jep22->last_state_sent)
      return;
    jep22->last_state_sent = jep22_state;
  }

  x = jutil_msgnew(TMSG_CHAT, (char*)jid, NULL, NULL);

  event = xmlnode_insert_tag(x, "x");
  xmlnode_put_attrib(event, "xmlns", NS_EVENT);
  if (type == ROSTER_EVENT_DELIVERED)
    xmlnode_insert_tag(event, "delivered");
  else if (type == ROSTER_EVENT_COMPOSING)
    xmlnode_insert_tag(event, "composing");
  xmlnode_put_attrib(event, "id", msgid);

  jab_send(jc, x);
  xmlnode_free(x);

  jb_reset_keepalive();
}
#endif

//  jb_send_chatstate(buddy, state)
// Send a chatstate or event (JEP-22/85) according to the buddy's capabilities.
// The message is sent to one of the resources with the highest priority.
#if defined JEP0022 || defined JEP0085
void jb_send_chatstate(gpointer buddy, guint chatstate)
{
  const char *jid;
  struct jep0085 *jep85 = NULL;
  struct jep0022 *jep22 = NULL;

  jid = buddy_getjid(buddy);
  if (!jid) return;

#ifdef JEP0085
  jep85 = buddy_resource_jep85(buddy, NULL);
  if (jep85 && jep85->support == CHATSTATES_SUPPORT_OK) {
    jb_send_jep85_chatstate(jid, chatstate);
    return;
  }
#endif
#ifdef JEP0022
  jep22 = buddy_resource_jep22(buddy, NULL);
  if (jep22 && jep22->support == CHATSTATES_SUPPORT_OK) {
    jb_send_jep22_event(jid, chatstate);
  }
#endif
}
#endif

//  chatstates_reset_probed(fulljid)
// If the JEP has been probed for this contact, set it back to unknown so
// that we probe it again.  The parameter must be a full jid (w/ resource).
#if defined JEP0022 || defined JEP0085
static void chatstates_reset_probed(const char *fulljid)
{
  char *rname, *barejid;
  GSList *sl_buddy;
  struct jep0085 *jep85;
  struct jep0022 *jep22;

  rname = strchr(fulljid, JID_RESOURCE_SEPARATOR);
  if (!rname++)
    return;

  barejid = jidtodisp(fulljid);
  sl_buddy = roster_find(barejid, jidsearch, ROSTER_TYPE_USER);
  g_free(barejid);

  if (!sl_buddy)
    return;

  jep85 = buddy_resource_jep85(sl_buddy->data, rname);
  jep22 = buddy_resource_jep22(sl_buddy->data, rname);

  if (jep85 && jep85->support == CHATSTATES_SUPPORT_PROBED)
    jep85->support = CHATSTATES_SUPPORT_UNKNOWN;
  if (jep22 && jep22->support == CHATSTATES_SUPPORT_PROBED)
    jep22->support = CHATSTATES_SUPPORT_UNKNOWN;
}
#endif

//  jb_subscr_send_auth(jid)
// Allow jid to receive our presence updates
void jb_subscr_send_auth(const char *jid)
{
  xmlnode x;

  x = jutil_presnew(JPACKET__SUBSCRIBED, (char *)jid, NULL);
  jab_send(jc, x);
  xmlnode_free(x);
}

//  jb_subscr_cancel_auth(jid)
// Cancel jid's subscription to our presence updates
void jb_subscr_cancel_auth(const char *jid)
{
  xmlnode x;

  x = jutil_presnew(JPACKET__UNSUBSCRIBED, (char *)jid, NULL);
  jab_send(jc, x);
  xmlnode_free(x);
}

//  jb_subscr_request_auth(jid)
// Request a subscription to jid's presence updates
void jb_subscr_request_auth(const char *jid)
{
  xmlnode x;

  x = jutil_presnew(JPACKET__SUBSCRIBE, (char *)jid, NULL);
  jab_send(jc, x);
  xmlnode_free(x);
}

//  jb_subscr_request_cancel(jid)
// Request to cancel jour subscription to jid's presence updates
void jb_subscr_request_cancel(const char *jid)
{
  xmlnode x;

  x = jutil_presnew(JPACKET__UNSUBSCRIBE, (char *)jid, NULL);
  jab_send(jc, x);
  xmlnode_free(x);
}

// Note: the caller should check the jid is correct
void jb_addbuddy(const char *jid, const char *name, const char *group)
{
  xmlnode y, z;
  eviqs *iqn;
  char *cleanjid;

  if (!online) return;

  cleanjid = jidtodisp(jid);

  // We don't check if the jabber user already exists in the roster,
  // because it allows to re-ask for notification.

  iqn = iqs_new(JPACKET__SET, NS_ROSTER, NULL, IQS_DEFAULT_TIMEOUT);
  y = xmlnode_insert_tag(xmlnode_get_tag(iqn->xmldata, "query"), "item");

  xmlnode_put_attrib(y, "jid", cleanjid);

  if (name)
    xmlnode_put_attrib(y, "name", name);

  if (group) {
    z = xmlnode_insert_tag(y, "group");
    xmlnode_insert_cdata(z, group, (unsigned) -1);
  }

  jab_send(jc, iqn->xmldata);
  iqs_del(iqn->id); // XXX

  jb_subscr_request_auth(cleanjid);

  roster_add_user(cleanjid, name, group, ROSTER_TYPE_USER, sub_pending);
  g_free(cleanjid);
  buddylist_build();

  update_roster = TRUE;
}

void jb_delbuddy(const char *jid)
{
  xmlnode y, z;
  eviqs *iqn;
  char *cleanjid;

  if (!online) return;

  cleanjid = jidtodisp(jid);

  // If the current buddy is an agent, unsubscribe from it
  if (roster_gettype(cleanjid) == ROSTER_TYPE_AGENT) {
    scr_LogPrint(LPRINT_LOGNORM, "Unregistering from the %s agent", cleanjid);

    iqn = iqs_new(JPACKET__SET, NS_REGISTER, NULL, IQS_DEFAULT_TIMEOUT);
    xmlnode_put_attrib(iqn->xmldata, "to", cleanjid);
    y = xmlnode_get_tag(iqn->xmldata, "query");
    xmlnode_insert_tag(y, "remove");
    jab_send(jc, iqn->xmldata);
    iqs_del(iqn->id); // XXX
  }

  // Cancel the subscriptions
  jb_subscr_cancel_auth(cleanjid);    // Cancel "from"
  jb_subscr_request_cancel(cleanjid); // Cancel "to"

  // Ask for removal from roster
  iqn = iqs_new(JPACKET__SET, NS_ROSTER, NULL, IQS_DEFAULT_TIMEOUT);
  y = xmlnode_get_tag(iqn->xmldata, "query");
  z = xmlnode_insert_tag(y, "item");
  xmlnode_put_attrib(z, "jid", cleanjid);
  xmlnode_put_attrib(z, "subscription", "remove");
  jab_send(jc, iqn->xmldata);
  iqs_del(iqn->id); // XXX

  roster_del_user(cleanjid);
  g_free(cleanjid);
  buddylist_build();

  update_roster = TRUE;
}

void jb_updatebuddy(const char *jid, const char *name, const char *group)
{
  xmlnode y;
  eviqs *iqn;
  char *cleanjid;

  if (!online) return;

  // XXX We should check name's and group's correctness

  cleanjid = jidtodisp(jid);

  iqn = iqs_new(JPACKET__SET, NS_ROSTER, NULL, IQS_DEFAULT_TIMEOUT);
  y = xmlnode_insert_tag(xmlnode_get_tag(iqn->xmldata, "query"), "item");
  xmlnode_put_attrib(y, "jid", cleanjid);
  xmlnode_put_attrib(y, "name", name);

  if (group) {
    y = xmlnode_insert_tag(y, "group");
    xmlnode_insert_cdata(y, group, (unsigned) -1);
  }

  jab_send(jc, iqn->xmldata);
  iqs_del(iqn->id); // XXX
  g_free(cleanjid);
}

void jb_request(const char *jid, enum iqreq_type reqtype)
{
  GSList *resources;
  GSList *roster_elt;
  void (*request_fn)(const char *);
  const char *strreqtype;

  if (reqtype == iqreq_version) {
    request_fn = &request_version;
    strreqtype = "version";
  } else if (reqtype == iqreq_time) {
    request_fn = &request_time;
    strreqtype = "time";
  } else if (reqtype == iqreq_last) {
    request_fn = &request_last;
    strreqtype = "last";
  } else if (reqtype == iqreq_vcard) {
    // Special case
  } else
    return;

  // vCard request
  if (reqtype == iqreq_vcard) {
    char *bjid = jidtodisp(jid);
    request_vcard(bjid);
    scr_LogPrint(LPRINT_NORMAL, "Sent vCard request to <%s>", bjid);
    g_free(bjid);
    return;
  }

  if (strchr(jid, JID_RESOURCE_SEPARATOR)) {
    // This is a full JID
    (*request_fn)(jid);
    scr_LogPrint(LPRINT_NORMAL, "Sent %s request to <%s>", strreqtype, jid);
    return;
  }

  // The resource has not been specified
  roster_elt = roster_find(jid, jidsearch, ROSTER_TYPE_USER|ROSTER_TYPE_ROOM);
  if (!roster_elt) {
    scr_LogPrint(LPRINT_NORMAL, "No known resource for <%s>...", jid);
    (*request_fn)(jid); // Let's send a request anyway...
    scr_LogPrint(LPRINT_NORMAL, "Sent %s request to <%s>", strreqtype, jid);
    return;
  }

  // Send a request to each resource
  resources = buddy_getresources(roster_elt->data);
  if (!resources) {
    scr_LogPrint(LPRINT_NORMAL, "No known resource for <%s>...", jid);
    (*request_fn)(jid); // Let's send a request anyway...
    scr_LogPrint(LPRINT_NORMAL, "Sent %s request to <%s>", strreqtype, jid);
  }
  for ( ; resources ; resources = g_slist_next(resources) ) {
    gchar *fulljid;
    fulljid = g_strdup_printf("%s/%s", jid, (char*)resources->data);
    (*request_fn)(fulljid);
    scr_LogPrint(LPRINT_NORMAL, "Sent %s request to <%s>", strreqtype, fulljid);
    g_free(fulljid);
  }
}

// Join a MUC room
void jb_room_join(const char *room, const char *nickname, const char *passwd)
{
  xmlnode x, y;
  gchar *roomid;
  GSList *room_elt;

  if (!online || !room) return;
  if (!nickname)        return;

  roomid = g_strdup_printf("%s/%s", room, nickname);
  if (check_jid_syntax(roomid)) {
    scr_LogPrint(LPRINT_NORMAL, "<%s/%s> is not a valid Jabber room", room,
                 nickname);
    g_free(roomid);
    return;
  }

  room_elt = roster_find(room, jidsearch, ROSTER_TYPE_USER|ROSTER_TYPE_ROOM);
  // Add room if it doesn't already exist
  if (!room_elt) {
    room_elt = roster_add_user(room, NULL, NULL, ROSTER_TYPE_ROOM, sub_none);
  } else {
    // Make sure this is a room (it can be a conversion user->room)
    buddy_settype(room_elt->data, ROSTER_TYPE_ROOM);
  }
  // If insideroom is TRUE, this is a nickname change and we don't care here
  if (!buddy_getinsideroom(room_elt->data)) {
    // We're trying to enter a room
    buddy_setnickname(room_elt->data, nickname);
  }

  // Send the XML request
  x = presnew(mystatus, roomid, mystatusmsg);
  y = xmlnode_insert_tag(x, "x");
  xmlnode_put_attrib(y, "xmlns", "http://jabber.org/protocol/muc");
  if (passwd) {
    xmlnode_insert_cdata(xmlnode_insert_tag(y, "password"), passwd,
                         (unsigned) -1);
  }

  jab_send(jc, x);
  xmlnode_free(x);
  jb_reset_keepalive();
  g_free(roomid);
}

// Unlock a MUC room
// room syntax: "room@server"
void jb_room_unlock(const char *room)
{
  xmlnode y, z;
  eviqs *iqn;

  if (!online || !room) return;

  iqn = iqs_new(JPACKET__SET, "http://jabber.org/protocol/muc#owner",
                "unlock", IQS_DEFAULT_TIMEOUT);
  xmlnode_put_attrib(iqn->xmldata, "to", room);
  y = xmlnode_get_tag(iqn->xmldata, "query");
  z = xmlnode_insert_tag(y, "x");
  xmlnode_put_attrib(z, "xmlns", "jabber:x:data");
  xmlnode_put_attrib(z, "type", "submit");

  jab_send(jc, iqn->xmldata);
  iqs_del(iqn->id); // XXX
  jb_reset_keepalive();
}

// Destroy a MUC room
// room syntax: "room@server"
void jb_room_destroy(const char *room, const char *venue, const char *reason)
{
  xmlnode y, z;
  eviqs *iqn;

  if (!online || !room) return;

  iqn = iqs_new(JPACKET__SET, "http://jabber.org/protocol/muc#owner",
                "destroy", IQS_DEFAULT_TIMEOUT);
  xmlnode_put_attrib(iqn->xmldata, "to", room);
  y = xmlnode_get_tag(iqn->xmldata, "query");
  z = xmlnode_insert_tag(y, "destroy");

  if (venue && *venue)
    xmlnode_put_attrib(z, "jid", venue);

  if (reason) {
    y = xmlnode_insert_tag(z, "reason");
    xmlnode_insert_cdata(y, reason, (unsigned) -1);
  }

  jab_send(jc, iqn->xmldata);
  iqs_del(iqn->id); // XXX
  jb_reset_keepalive();
}

// Change role or affiliation of a MUC user
// room syntax: "room@server"
// Either the jid or the nickname must be set (when banning, only the jid is
// allowed)
// ra: new role or affiliation
//     (ex. role none for kick, affil outcast for ban...)
// The reason can be null
// Return 0 if everything is ok
int jb_room_setattrib(const char *roomid, const char *jid, const char *nick,
                      struct role_affil ra, const char *reason)
{
  xmlnode y, z;
  eviqs *iqn;

  if (!online || !roomid) return 1;
  if (!jid && !nick) return 1;

  if (check_jid_syntax((char*)roomid)) {
    scr_LogPrint(LPRINT_NORMAL, "<%s> is not a valid Jabber id", roomid);
    return 1;
  }
  if (jid && check_jid_syntax((char*)jid)) {
    scr_LogPrint(LPRINT_NORMAL, "<%s> is not a valid Jabber id", jid);
    return 1;
  }

  if (ra.type == type_affil && ra.val.affil == affil_outcast && !jid)
    return 1; // Shouldn't happen (jid mandatory when banning)

  iqn = iqs_new(JPACKET__SET, "http://jabber.org/protocol/muc#admin",
                "roleaffil", IQS_DEFAULT_TIMEOUT);
  xmlnode_put_attrib(iqn->xmldata, "to", roomid);
  xmlnode_put_attrib(iqn->xmldata, "type", "set");
  y = xmlnode_get_tag(iqn->xmldata, "query");
  z = xmlnode_insert_tag(y, "item");

  if (jid) {
    xmlnode_put_attrib(z, "jid", jid);
  } else { // nickname
    xmlnode_put_attrib(z, "nick", nick);
  }

  if (ra.type == type_affil)
    xmlnode_put_attrib(z, "affiliation", straffil[ra.val.affil]);
  else if (ra.type == type_role)
    xmlnode_put_attrib(z, "role", strrole[ra.val.role]);

  if (reason) {
    y = xmlnode_insert_tag(z, "reason");
    xmlnode_insert_cdata(y, reason, (unsigned) -1);
  }

  jab_send(jc, iqn->xmldata);
  iqs_del(iqn->id); // XXX
  jb_reset_keepalive();

  return 0;
}

// Invite a user to a MUC room
// room syntax: "room@server"
// reason can be null.
void jb_room_invite(const char *room, const char *jid, const char *reason)
{
  xmlnode x, y, z;

  if (!online || !room || !jid) return;

  x = jutil_msgnew(NULL, (char*)room, NULL, NULL);

  y = xmlnode_insert_tag(x, "x");
  xmlnode_put_attrib(y, "xmlns", "http://jabber.org/protocol/muc#user");

  z = xmlnode_insert_tag(y, "invite");
  xmlnode_put_attrib(z, "to", jid);

  if (reason) {
    y = xmlnode_insert_tag(z, "reason");
    xmlnode_insert_cdata(y, reason, (unsigned) -1);
  }

  jab_send(jc, x);
  xmlnode_free(x);
  jb_reset_keepalive();
}

//  jb_set_storage_bookmark(roomid, name, nick, passwd, autojoin)
// Update the private storage bookmarks: add a conference room.
// If name is nil, we remove the bookmark.
void jb_set_storage_bookmark(const char *roomid, const char *name,
                             const char *nick, const char *passwd, int autojoin)
{
  xmlnode x;

  if (!roomid)
    return;

  // If we have no bookmarks, probably the server doesn't support them.
  if (!bookmarks) {
    scr_LogPrint(LPRINT_LOGNORM,
                 "Sorry, your server doesn't seem to support private storage.");
    return;
  }

  // Walk through the storage tags
  x = xmlnode_get_firstchild(bookmarks);
  for ( ; x; x = xmlnode_get_nextsibling(x)) {
    const char *jid;
    const char *p;
    p = xmlnode_get_name(x);
    // If the current node is a conference item, see if we have to replace it.
    if (p && !strcmp(p, "conference")) {
      jid = xmlnode_get_attrib(x, "jid");
      if (!jid)
        continue;
      if (!strcmp(jid, roomid)) {
        // We've found a bookmark for this room.  Let's hide it and we'll
        // create a new one.
        xmlnode_hide(x);
        break;
      }
    }
  }

  // Let's create a node/bookmark for this roomid, if the name is not NULL.
  if (name) {
    x = xmlnode_insert_tag(bookmarks, "conference");
    xmlnode_put_attrib(x, "jid", roomid);
    xmlnode_put_attrib(x, "name", name);
    xmlnode_put_attrib(x, "autojoin", autojoin ? "1" : "0");
    if (nick)
      xmlnode_insert_cdata(xmlnode_insert_tag(x, "nick"), nick, -1);
    if (passwd)
      xmlnode_insert_cdata(xmlnode_insert_tag(x, "password"), passwd, -1);
  }

  if (online)
    send_storage_bookmarks();
  else
    scr_LogPrint(LPRINT_LOGNORM,
                 "Warning: you're not connected to the server.");
}

//  jb_get_storage_rosternotes(barejid)
// Return thenote associated to this jid.
// The caller should g_free the string after use.
char *jb_get_storage_rosternotes(const char *barejid)
{
  xmlnode x;

  if (!barejid)
    return NULL;

  // If we have no rosternotes, probably the server doesn't support them.
  if (!rosternotes) {
    scr_LogPrint(LPRINT_LOGNORM,
                 "Sorry, your server doesn't seem to support private storage.");
    return NULL;
  }

  // Walk through the storage tags
  x = xmlnode_get_firstchild(rosternotes);
  for ( ; x; x = xmlnode_get_nextsibling(x)) {
    const char *jid;
    const char *p;
    p = xmlnode_get_name(x);
    // If the current node is a conference item, see if we have to replace it.
    if (p && !strcmp(p, "note")) {
      jid = xmlnode_get_attrib(x, "jid");
      if (!jid)
        continue;
      if (!strcmp(jid, barejid)) {
        // We've found a note for this contact.
        return g_strdup(xmlnode_get_data(x));
      }
    }
  }
  return NULL;  // No note found
}

//  jb_set_storage_rosternotes(barejid, note)
// Update the private storage rosternotes: add/delete a note.
// If note is nil, we remove the existing note.
void jb_set_storage_rosternotes(const char *barejid, const char *note)
{
  xmlnode x;

  if (!barejid)
    return;

  // If we have no rosternotes, probably the server doesn't support them.
  if (!rosternotes) {
    scr_LogPrint(LPRINT_LOGNORM,
                 "Sorry, your server doesn't seem to support private storage.");
    return;
  }

  // Walk through the storage tags
  x = xmlnode_get_firstchild(rosternotes);
  for ( ; x; x = xmlnode_get_nextsibling(x)) {
    const char *jid;
    const char *p;
    p = xmlnode_get_name(x);
    // If the current node is a conference item, see if we have to replace it.
    if (p && !strcmp(p, "note")) {
      jid = xmlnode_get_attrib(x, "jid");
      if (!jid)
        continue;
      if (!strcmp(jid, barejid)) {
        // We've found a note for this jid.  Let's hide it and we'll
        // create a new one.
        xmlnode_hide(x);
        break;
      }
    }
  }

  // Let's create a node for this jid, if the note is not NULL.
  if (note) {
    x = xmlnode_insert_tag(rosternotes, "note");
    xmlnode_put_attrib(x, "jid", barejid);
    xmlnode_insert_cdata(x, note, -1);
  }

  if (online)
    send_storage_rosternotes();
  else
    scr_LogPrint(LPRINT_LOGNORM,
                 "Warning: you're not connected to the server.");
}

static void gotmessage(char *type, const char *from, const char *body,
                       const char *enc, time_t timestamp)
{
  char *jid;
  const char *rname, *s;

  jid = jidtodisp(from);

  rname = strchr(from, JID_RESOURCE_SEPARATOR);
  if (rname) rname++;

  // Check for unexpected groupchat messages
  // If we receive a groupchat message from a room we're not a member of,
  // this is probably a server issue and the best we can do is to send
  // a type unavailable.
  if (type && !strcmp(type, "groupchat") && !roster_getnickname(jid)) {
    // It shouldn't happen, probably a server issue
    GSList *room_elt;
    char *mbuf;

    mbuf = g_strdup_printf("Unexpected groupchat packet!");
    scr_LogPrint(LPRINT_LOGNORM, "%s", mbuf);
    scr_WriteIncomingMessage(jid, mbuf, 0, HBB_PREFIX_INFO);
    g_free(mbuf);

    // Send back an unavailable packet
    jb_setstatus(offline, jid, "");

    // MUC
    // Make sure this is a room (it can be a conversion user->room)
    room_elt = roster_find(jid, jidsearch, 0);
    if (!room_elt) {
      room_elt = roster_add_user(jid, NULL, NULL, ROSTER_TYPE_ROOM, sub_none);
    } else {
      buddy_settype(room_elt->data, ROSTER_TYPE_ROOM);
    }

    g_free(jid);

    buddylist_build();
    scr_DrawRoster();
    return;
  }

  // We don't call the message_in hook if 'block_unsubscribed' is true and
  // this is a regular message from an unsubscribed user.
  // System messages (from our server) are allowed.
  if (!settings_opt_get_int("block_unsubscribed") ||
      (roster_getsubscription(jid) & sub_from) ||
      (type && strcmp(type, "chat")) ||
      ((s = settings_opt_get("server")) != NULL && !strcasecmp(jid, s))) {
    hk_message_in(jid, rname, timestamp, body, type);
  } else {
    scr_LogPrint(LPRINT_LOGNORM, "Blocked a message from <%s>", jid);
  }
  g_free(jid);
}

static const char *defaulterrormsg(int code)
{
  const char *desc;

  switch(code) {
    case 401: desc = "Unauthorized";
              break;
    case 302: desc = "Redirect";
              break;
    case 400: desc = "Bad request";
              break;
    case 402: desc = "Payment Required";
              break;
    case 403: desc = "Forbidden";
              break;
    case 404: desc = "Not Found";
              break;
    case 405: desc = "Not Allowed";
              break;
    case 406: desc = "Not Acceptable";
              break;
    case 407: desc = "Registration Required";
              break;
    case 408: desc = "Request Timeout";
              break;
    case 409: desc = "Conflict";
              break;
    case 500: desc = "Internal Server Error";
              break;
    case 501: desc = "Not Implemented";
              break;
    case 502: desc = "Remote Server Error";
              break;
    case 503: desc = "Service Unavailable";
              break;
    case 504: desc = "Remote Server Timeout";
              break;
    default:
              desc = NULL;
  }

  return desc;
}

//  display_server_error(x)
// Display the error to the user
// x: error tag xmlnode pointer
void display_server_error(xmlnode x)
{
  const char *desc = NULL;
  char *sdesc;
  int code = 0;
  char *s;
  const char *p;

  /* RFC3920:
   *    The <error/> element:
   *       o  MUST contain a child element corresponding to one of the defined
   *          stanza error conditions specified below; this element MUST be
   *          qualified by the 'urn:ietf:params:xml:ns:xmpp-stanzas' namespace.
   */
  p = xmlnode_get_name(xmlnode_get_firstchild(x));
  if (p)
    scr_LogPrint(LPRINT_LOGNORM, "Received error packet [%s]", p);

  // For backward compatibility
  if ((s = xmlnode_get_attrib(x, "code")) != NULL) {
    code = atoi(s);
    // Default message
    desc = defaulterrormsg(code);
  }

  // Error tag data is better, if available
  s = xmlnode_get_data(x);
  if (s && *s) desc = s;

  // And sometimes there is a text message
  s = xmlnode_get_tag_data(x, "text");
  if (s && *s) desc = s;

  // Strip trailing newlines
  sdesc = g_strdup(desc);
  for (s = sdesc; *s; s++) ;
  if (s > sdesc)
    s--;
  while (s >= sdesc && (*s == '\n' || *s == '\r'))
    *s-- = '\0';

  scr_LogPrint(LPRINT_LOGNORM, "Error code from server: %d %s", code, sdesc);
  g_free(sdesc);
}

static void statehandler(jconn conn, int state)
{
  static int previous_state = -1;

  scr_LogPrint(LPRINT_DEBUG, "StateHandler called (state=%d).", state);

  switch(state) {
    case JCONN_STATE_OFF:
        if (previous_state != JCONN_STATE_OFF)
          scr_LogPrint(LPRINT_LOGNORM, "[Jabber] Not connected to the server");

        // Sometimes the state isn't correctly updated
        if (jc)
          jc->state = JCONN_STATE_OFF;
        online = FALSE;
        mystatus = offline;
        // Free bookmarks
        xmlnode_free(bookmarks);
        bookmarks = NULL;
        // Free roster
        roster_free();
        // Update display
        update_roster = TRUE;
        scr_UpdateBuddyWindow();
        break;

    case JCONN_STATE_CONNECTED:
        scr_LogPrint(LPRINT_LOGNORM, "[Jabber] Connected to the server");
        break;

    case JCONN_STATE_AUTH:
        scr_LogPrint(LPRINT_LOGNORM, "[Jabber] Authenticating to the server");
        break;

    case JCONN_STATE_ON:
        scr_LogPrint(LPRINT_LOGNORM, "[Jabber] Communication with the server "
                     "established");
        online = TRUE;
        // We set AutoConnection to true after the 1st successful connection
        AutoConnection = true;
        break;

    case JCONN_STATE_CONNECTING:
        if (previous_state != state)
          scr_LogPrint(LPRINT_LOGNORM, "[Jabber] Connecting to the server");
        break;

    default:
        break;
  }
  previous_state = state;
}

inline static xmlnode xml_get_xmlns(xmlnode xmldata, const char *xmlns)
{
  xmlnode x;
  char *p;

  x = xmlnode_get_firstchild(xmldata);
  for ( ; x; x = xmlnode_get_nextsibling(x)) {
    if ((p = xmlnode_get_attrib(x, "xmlns")) && !strcmp(p, xmlns))
      break;
  }
  return x;
}

static time_t xml_get_timestamp(xmlnode xmldata)
{
  xmlnode x;
  char *p;

  x = xml_get_xmlns(xmldata, NS_DELAY);
  if ((p = xmlnode_get_attrib(x, "stamp")) != NULL)
    return from_iso8601(p, 1);
  return 0;
}

static void handle_presence_muc(const char *from, xmlnode xmldata,
                                const char *roomjid, const char *rname,
                                enum imstatus ust, char *ustmsg,
                                time_t usttime, char bpprio)
{
  xmlnode y;
  char *p;
  char *mbuf;
  const char *ournick;
  enum imrole mbrole = role_none;
  enum imaffiliation mbaffil = affil_none;
  const char *mbjid = NULL, *mbnick = NULL;
  const char *actorjid = NULL, *reason = NULL;
  bool new_member = FALSE; // True if somebody else joins the room (not us)
  unsigned int statuscode = 0;
  GSList *room_elt;
  int log_muc_conf;
  guint msgflags;

  log_muc_conf = settings_opt_get_int("log_muc_conf");

  room_elt = roster_find(roomjid, jidsearch, 0);
  if (!room_elt) {
    // Add room if it doesn't already exist
    // It shouldn't happen, there is probably something wrong (server or
    // network issue?)
    room_elt = roster_add_user(roomjid, NULL, NULL, ROSTER_TYPE_ROOM, sub_none);
    scr_LogPrint(LPRINT_LOGNORM, "Strange MUC presence message");
  } else {
    // Make sure this is a room (it can be a conversion user->room)
    buddy_settype(room_elt->data, ROSTER_TYPE_ROOM);
  }

  // Get room member's information
  y = xmlnode_get_tag(xmldata, "item");
  if (y) {
    xmlnode z;
    p = xmlnode_get_attrib(y, "affiliation");
    if (p) {
      if (!strcmp(p, "owner"))        mbaffil = affil_owner;
      else if (!strcmp(p, "admin"))   mbaffil = affil_admin;
      else if (!strcmp(p, "member"))  mbaffil = affil_member;
      else if (!strcmp(p, "outcast")) mbaffil = affil_outcast;
      else if (!strcmp(p, "none"))    mbaffil = affil_none;
      else scr_LogPrint(LPRINT_LOGNORM, "<%s>: Unknown affiliation \"%s\"",
                        from, p);
    }
    p = xmlnode_get_attrib(y, "role");
    if (p) {
      if (!strcmp(p, "moderator"))        mbrole = role_moderator;
      else if (!strcmp(p, "participant")) mbrole = role_participant;
      else if (!strcmp(p, "visitor"))     mbrole = role_visitor;
      else if (!strcmp(p, "none"))        mbrole = role_none;
      else scr_LogPrint(LPRINT_LOGNORM, "<%s>: Unknown role \"%s\"",
                        from, p);
    }
    mbjid = xmlnode_get_attrib(y, "jid");
    mbnick = xmlnode_get_attrib(y, "nick");
    // For kick/ban, there can be actor and reason tags
    reason = xmlnode_get_tag_data(y, "reason");
    z = xmlnode_get_tag(y, "actor");
    if (z)
      actorjid = xmlnode_get_attrib(z, "jid");
  }

  // Get our room nickname
  ournick = buddy_getnickname(room_elt->data);

  if (!ournick) {
    // It shouldn't happen, probably a server issue
    mbuf = g_strdup_printf("Unexpected groupchat packet!");

    scr_LogPrint(LPRINT_LOGNORM, "%s", mbuf);
    scr_WriteIncomingMessage(roomjid, mbuf, 0, HBB_PREFIX_INFO);
    g_free(mbuf);
    // Send back an unavailable packet
    jb_setstatus(offline, roomjid, "");
    scr_DrawRoster();
    return;
  }

  // Get the status code
  // 201: a room has been created
  // 301: the user has been banned from the room
  // 303: new room nickname
  // 307: the user has been kicked from the room
  // 321,322,332: the user has been removed from the room
  y = xmlnode_get_tag(xmldata, "status");
  if (y) {
    p = xmlnode_get_attrib(y, "code");
    if (p)
      statuscode = atoi(p);
  }

  // Check for nickname change
  if (statuscode == 303 && mbnick) {
    mbuf = g_strdup_printf("%s is now known as %s", rname, mbnick);
    scr_WriteIncomingMessage(roomjid, mbuf, usttime,
                             HBB_PREFIX_INFO|HBB_PREFIX_NOFLAG);
    if (log_muc_conf) hlog_write_message(roomjid, 0, FALSE, mbuf);
    g_free(mbuf);
    buddy_resource_setname(room_elt->data, rname, mbnick);
    // Maybe it's _our_ nickname...
    if (ournick && !strcmp(rname, ournick))
      buddy_setnickname(room_elt->data, mbnick);
  }

  // Check for departure/arrival
  if (!mbnick && mbrole == role_none) {
    enum { leave=0, kick, ban } how = leave;
    bool we_left = FALSE;

    if (statuscode == 307)
      how = kick;
    else if (statuscode == 301)
      how = ban;

    // If this is a leave, check if it is ourself
    if (ournick && !strcmp(rname, ournick)) {
      we_left = TRUE; // _We_ have left! (kicked, banned, etc.)
      buddy_setinsideroom(room_elt->data, FALSE);
      buddy_setnickname(room_elt->data, NULL);
      buddy_del_all_resources(room_elt->data);
      buddy_settopic(room_elt->data, NULL);
      scr_UpdateChatStatus(FALSE);
      update_roster = TRUE;
    }

    // The message depends on _who_ left, and _how_
    if (how) {
      gchar *mbuf_end;
      // Forced leave
      if (actorjid) {
        mbuf_end = g_strdup_printf("%s from %s by <%s>.\nReason: %s",
                                   (how == ban ? "banned" : "kicked"),
                                   roomjid, actorjid, reason);
      } else {
        mbuf_end = g_strdup_printf("%s from %s.",
                                   (how == ban ? "banned" : "kicked"),
                                   roomjid);
      }
      if (we_left)
        mbuf = g_strdup_printf("You have been %s", mbuf_end);
      else
        mbuf = g_strdup_printf("%s has been %s", rname, mbuf_end);

      g_free(mbuf_end);
    } else {
      // Natural leave
      if (we_left) {
        xmlnode destroynode = xmlnode_get_tag(xmldata, "destroy");
        if (destroynode) {
          if ((reason = xmlnode_get_tag_data(destroynode, "reason"))) {
            mbuf = g_strdup_printf("You have left %s, "
                                   "the room has been destroyed: %s",
                                   roomjid, reason);
          } else {
            mbuf = g_strdup_printf("You have left %s, "
                                   "the room has been destroyed", roomjid);
          }
        } else {
          mbuf = g_strdup_printf("You have left %s", roomjid);
        }
      } else {
        if (ust != offline) {
          // This can happen when a network failure occurs,
          // this isn't an official leave but the user isn't there anymore.
          mbuf = g_strdup_printf("%s has disappeared!", rname);
          ust = offline;
        } else {
          if (ustmsg)
            mbuf = g_strdup_printf("%s has left: %s", rname, ustmsg);
          else
            mbuf = g_strdup_printf("%s has left", rname);
        }
      }
    }

    msgflags = HBB_PREFIX_INFO;
    if (!we_left)
      msgflags |= HBB_PREFIX_NOFLAG;

    scr_WriteIncomingMessage(roomjid, mbuf, usttime, msgflags);

    if (log_muc_conf) hlog_write_message(roomjid, 0, FALSE, mbuf);

    if (we_left) {
      scr_LogPrint(LPRINT_LOGNORM, "%s", mbuf);
      g_free(mbuf);
      return;
    }
    g_free(mbuf);
  } else if (buddy_getstatus(room_elt->data, rname) == offline &&
             ust != offline) {

    if (!buddy_getinsideroom(room_elt->data)) {
      // We weren't inside the room yet.  Now we are.
      // However, this could be a presence packet from another room member

      buddy_setinsideroom(room_elt->data, TRUE);
      // Set the message flag unless we're already in the room buffer window
      scr_setmsgflag_if_needed(roomjid, FALSE);
      // Add a message to the tracelog file
      mbuf = g_strdup_printf("You have joined %s as \"%s\"", roomjid, ournick);
      scr_LogPrint(LPRINT_LOGNORM, "%s", mbuf);
      g_free(mbuf);
      mbuf = g_strdup_printf("You have joined as \"%s\"", ournick);

      // The 1st presence message could be for another room member
      if (strcmp(ournick, rname)) {
        // Display current mbuf and create a new message for the member
        // Note: the usttime timestamp is related to the other member,
        //       so we use 0 here.
        scr_WriteIncomingMessage(roomjid, mbuf, 0,
                                 HBB_PREFIX_INFO|HBB_PREFIX_NOFLAG);
        if (log_muc_conf) hlog_write_message(roomjid, 0, FALSE, mbuf);
        g_free(mbuf);
        mbuf = g_strdup_printf("%s has joined", rname);
        new_member = TRUE;
      }
    } else {
      if (strcmp(ournick, rname)) {
        mbuf = g_strdup_printf("%s has joined", rname);
        new_member = TRUE;
      } else
        mbuf = NULL;
    }

    if (mbuf) {
      scr_WriteIncomingMessage(roomjid, mbuf, usttime,
                               HBB_PREFIX_INFO|HBB_PREFIX_NOFLAG);
      if (log_muc_conf) hlog_write_message(roomjid, 0, FALSE, mbuf);
      g_free(mbuf);
    }
  }

  // Update room member status
  if (rname) {
    roster_setstatus(roomjid, rname, bpprio, ust, ustmsg, usttime,
                     mbrole, mbaffil, mbjid);
    if (new_member && settings_opt_get_int("muc_auto_whois")) {
      // FIXME: This will fail for some UTF-8 nicknames.
      gchar *joiner_nick = from_utf8(rname);
      room_whois(room_elt->data, joiner_nick, FALSE);
      g_free(joiner_nick);
    }
  } else
    scr_LogPrint(LPRINT_LOGNORM, "MUC DBG: no rname!"); /* DBG */

  scr_DrawRoster();
}

static void handle_packet_presence(jconn conn, char *type, char *from,
                                   xmlnode xmldata)
{
  char *p, *r;
  char *ustmsg;
  const char *rname;
  enum imstatus ust;
  char bpprio;
  time_t timestamp = 0L;
  xmlnode muc_packet;

  rname = strchr(from, JID_RESOURCE_SEPARATOR);
  if (rname) rname++;

  r = jidtodisp(from);

  // Check for MUC presence packet
  muc_packet = xml_get_xmlns(xmldata, "http://jabber.org/protocol/muc#user");

  if (type && !strcmp(type, TMSG_ERROR)) {
    xmlnode x;
    scr_LogPrint(LPRINT_LOGNORM, "Error presence packet from <%s>", r);
    if ((x = xmlnode_get_tag(xmldata, TMSG_ERROR)) != NULL)
      display_server_error(x);

    // Let's check it isn't a nickname conflict.
    // XXX Note: We should handle the <conflict/> string condition.
    if ((p = xmlnode_get_attrib(x, "code")) != NULL) {
      if (atoi(p) == 409) {
        // 409 = conlict (nickname is in use or registered by another user)
        // If we are not inside this room, we should reset the nickname
        GSList *room_elt = roster_find(r, jidsearch, 0);
        if (room_elt && !buddy_getinsideroom(room_elt->data))
          buddy_setnickname(room_elt->data, NULL);
      }
    }

    g_free(r);
    return;
  }

  p = xmlnode_get_tag_data(xmldata, "priority");
  if (p && *p) bpprio = (gchar)atoi(p);
  else         bpprio = 0;

  ust = available;
  p = xmlnode_get_tag_data(xmldata, "show");
  if (p) {
    if (!strcmp(p, "away"))      ust = away;
    else if (!strcmp(p, "dnd"))  ust = dontdisturb;
    else if (!strcmp(p, "xa"))   ust = notavail;
    else if (!strcmp(p, "chat")) ust = freeforchat;
  }

  if (type && !strcmp(type, "unavailable"))
    ust = offline;

  ustmsg = xmlnode_get_tag_data(xmldata, "status");

  // Timestamp?
  timestamp = xml_get_timestamp(xmldata);

  if (muc_packet) {
    // This is a MUC presence message
    handle_presence_muc(from, muc_packet, r, rname,
                        ust, ustmsg, timestamp, bpprio);
  } else {
    // Not a MUC message, so this is a regular buddy...
    // Call hk_statuschange() if status has changed or if the
    // status message is different
    const char *m = roster_getstatusmsg(r, rname);
    if ((ust != roster_getstatus(r, rname)) ||
        (!ustmsg && m && m[0]) || (ustmsg && (!m || strcmp(ustmsg, m))))
      hk_statuschange(r, rname, bpprio, timestamp, ust, ustmsg);
  }

  g_free(r);
}

static void handle_packet_message(jconn conn, char *type, char *from,
                                  xmlnode xmldata)
{
  char *p, *r, *s;
  xmlnode x;
  char *body = NULL;
  char *enc = NULL;
  char *tmp = NULL;
  time_t timestamp = 0L;

  body = xmlnode_get_tag_data(xmldata, "body");

  p = xmlnode_get_tag_data(xmldata, "subject");
  if (p != NULL) {
    if (type && !strcmp(type, TMSG_GROUPCHAT)) {  // Room topic
      GSList *roombuddy;
      gchar *mbuf;
      gchar *subj = p;
      // Get the room (s) and the nickname (r)
      s = g_strdup(from);
      r = strchr(s, JID_RESOURCE_SEPARATOR);
      if (r) *r++ = 0;
      else   r = s;
      // Set the new topic
      roombuddy = roster_find(s, jidsearch, 0);
      if (roombuddy)
        buddy_settopic(roombuddy->data, subj);
      // Display inside the room window
      if (r == s) {
        // No specific resource (this is certainly history)
        mbuf = g_strdup_printf("The topic has been set to: %s", subj);
      } else {
        mbuf = g_strdup_printf("%s has set the topic to: %s", r, subj);
      }
      scr_WriteIncomingMessage(s, mbuf, 0,
                               HBB_PREFIX_INFO|HBB_PREFIX_NOFLAG);
      if (settings_opt_get_int("log_muc_conf"))
        hlog_write_message(s, 0, FALSE, mbuf);
      g_free(s);
      g_free(mbuf);
      // The topic is displayed in the chat status line, so refresh now.
      scr_UpdateChatStatus(TRUE);
    } else {                                      // Chat message
      tmp = g_new(char, (body ? strlen(body) : 0) + strlen(p) + 4);
      *tmp = '[';
      strcpy(tmp+1, p);
      strcat(tmp, "]\n");
      if (body) strcat(tmp, body);
      body = tmp;
    }
  }

  handle_state_events(from, xmldata);

  // Not used yet...
  x = xml_get_xmlns(xmldata, NS_ENCRYPTED);
  if (x && (p = xmlnode_get_data(x)) != NULL) {
    enc = p;
  }

  // Timestamp?
  timestamp = xml_get_timestamp(xmldata);

  if (type && !strcmp(type, TMSG_ERROR)) {
    if ((x = xmlnode_get_tag(xmldata, TMSG_ERROR)) != NULL)
      display_server_error(x);
#if defined JEP0022 || defined JEP0085
    // If the JEP85/22 support is probed, set it back to unknown so that
    // we probe it again.
    chatstates_reset_probed(from);
#endif
  }
  if (from && body)
    gotmessage(type, from, body, enc, timestamp);
  g_free(tmp);
}

void handle_state_events(char *from, xmlnode xmldata)
{
#if defined JEP0022 || defined JEP0085
  xmlnode state_ns = NULL;
  const char *body;
  char *rname, *jid;
  GSList *sl_buddy;
  guint events;
  struct jep0022 *jep22 = NULL;
  struct jep0085 *jep85 = NULL;
  enum {
    JEP_none,
    JEP_85,
    JEP_22
  } which_jep = JEP_none;

  rname = strchr(from, JID_RESOURCE_SEPARATOR);
  jid   = jidtodisp(from);
  sl_buddy = roster_find(jid, jidsearch, ROSTER_TYPE_USER);

  /* XXX Actually that's wrong, since it filters out server "offline"
     messages (for JEP-0022).  This JEP is (almost) deprecated so
     we don't really care. */
  if (!sl_buddy || !rname++) {
    g_free(jid);
    return;
  }

  /* Let's see chich JEP the contact uses.  If possible, we'll use
     JEP-85, if not we'll look for JEP-22 support. */
  events = buddy_resource_getevents(sl_buddy->data, rname);

  jep85 = buddy_resource_jep85(sl_buddy->data, rname);
  if (jep85) {
    state_ns = xml_get_xmlns(xmldata, NS_CHATSTATES);
    if (state_ns)
      which_jep = JEP_85;
  }

  if (which_jep != JEP_85) { /* Fall back to JEP-0022 */
    jep22 = buddy_resource_jep22(sl_buddy->data, rname);
    if (jep22) {
      state_ns = xml_get_xmlns(xmldata, NS_EVENT);
      if (state_ns)
        which_jep = JEP_22;
    }
  }

  if (!which_jep) { /* Sender does not use chat states */
    g_free(jid);
    return;
  }

  body = xmlnode_get_tag_data(xmldata, "body");

  if (which_jep == JEP_85) { /* JEP-0085 */
    const char *p;
    jep85->support = CHATSTATES_SUPPORT_OK;

    p = xmlnode_get_name(state_ns);
    if (!strcmp(p, "composing")) {
      jep85->last_state_rcvd = ROSTER_EVENT_COMPOSING;
    } else if (!strcmp(p, "active")) {
      jep85->last_state_rcvd = ROSTER_EVENT_ACTIVE;
    } else if (!strcmp(p, "paused")) {
      jep85->last_state_rcvd = ROSTER_EVENT_PAUSED;
    } else if (!strcmp(p, "inactive")) {
      jep85->last_state_rcvd = ROSTER_EVENT_INACTIVE;
    } else if (!strcmp(p, "gone")) {
      jep85->last_state_rcvd = ROSTER_EVENT_GONE;
    }
    events = jep85->last_state_rcvd;
  } else {              /* JEP-0022 */
#ifdef JEP0022
    const char *msgid;
    jep22->support = CHATSTATES_SUPPORT_OK;
    jep22->last_state_rcvd = ROSTER_EVENT_NONE;

    msgid = xmlnode_get_attrib(xmldata, "id");

    if (xmlnode_get_tag(state_ns, "composing")) {
      // Clear composing if the message contains a body
      if (body)
        events &= ~ROSTER_EVENT_COMPOSING;
      else
        events |= ROSTER_EVENT_COMPOSING;
      jep22->last_state_rcvd |= ROSTER_EVENT_COMPOSING;

    } else {
      events &= ~ROSTER_EVENT_COMPOSING;
    }

    // Cache the message id
    g_free(jep22->last_msgid_rcvd);
    if (msgid)
      jep22->last_msgid_rcvd = g_strdup(msgid);
    else
      jep22->last_msgid_rcvd = NULL;

    if (xmlnode_get_tag(state_ns, "delivered")) {
      jep22->last_state_rcvd |= ROSTER_EVENT_DELIVERED;

      // Do we have to send back an ACK?
      if (body)
        jb_send_jep22_event(from, ROSTER_EVENT_DELIVERED);
    }
#endif
  }

  buddy_resource_setevents(sl_buddy->data, rname, events);

  update_roster = TRUE;

  g_free(jid);
#endif
}

static void evscallback_subscription(eviqs *evp, guint evcontext)
{
  char *barejid;
  char *buf;

  if (evcontext == EVS_CONTEXT_TIMEOUT) {
    scr_LogPrint(LPRINT_LOGNORM, "Event %s timed out, cancelled.",
                 evp->id);
    return;
  }
  if (evcontext == EVS_CONTEXT_CANCEL) {
    scr_LogPrint(LPRINT_LOGNORM, "Event %s cancelled.", evp->id);
    return;
  }
  if (!(evcontext & EVS_CONTEXT_USER))
    return;

  // Sanity check
  if (!evp->data) {
    // Shouldn't happen, data should be set to the barejid.
    scr_LogPrint(LPRINT_LOGNORM, "Error in evs callback.");
    return;
  }

  // Ok, let's work now.
  // evcontext: 0, 1 == reject, accept

  barejid = evp->data;

  if (evcontext & ~EVS_CONTEXT_USER) {
    // Accept subscription request
    jb_subscr_send_auth(barejid);
    buf = g_strdup_printf("<%s> is allowed to receive your presence updates",
                          barejid);
  } else {
    // Reject subscription request
    jb_subscr_cancel_auth(barejid);
    buf = g_strdup_printf("<%s> won't receive your presence updates", barejid);
    if (settings_opt_get_int("delete_on_reject")) {
      // Remove the buddy from the roster if there is no current subscription
      if (roster_getsubscription(barejid) == sub_none)
        jb_delbuddy(barejid);
    }
  }
  scr_WriteIncomingMessage(barejid, buf, 0, HBB_PREFIX_INFO);
  scr_LogPrint(LPRINT_LOGNORM, "%s", buf);
  g_free(buf);
}

static void handle_packet_s10n(jconn conn, char *type, char *from,
                               xmlnode xmldata)
{
  char *r;
  char *buf;
  int newbuddy;

  r = jidtodisp(from);

  newbuddy = !roster_find(r, jidsearch, 0);

  if (!strcmp(type, "subscribe")) {
    /* The sender wishes to subscribe to our presence */
    char *msg;
    int isagent;
    eviqs *evn;

    isagent = (roster_gettype(r) & ROSTER_TYPE_AGENT) != 0;
    msg = xmlnode_get_tag_data(xmldata, "status");

    buf = g_strdup_printf("<%s> wants to subscribe to your presence updates",
                          from);
    scr_WriteIncomingMessage(r, buf, 0, HBB_PREFIX_INFO);
    scr_LogPrint(LPRINT_LOGNORM, "%s", buf);
    g_free(buf);

    if (msg) {
      buf = g_strdup_printf("<%s> said: %s", from, msg);
      scr_WriteIncomingMessage(r, buf, 0, HBB_PREFIX_INFO);
      replace_nl_with_dots(buf);
      scr_LogPrint(LPRINT_LOGNORM, "%s", buf);
      g_free(buf);
    }

    // Create a new event item
    evn = evs_new(EVS_TYPE_SUBSCRIPTION, EVS_MAX_TIMEOUT);
    if (evn) {
      evn->callback = &evscallback_subscription;
      evn->data = g_strdup(r);
      evn->desc = g_strdup_printf("<%s> wants to subscribe to your "
                                  "presence updates", r);
      buf = g_strdup_printf("Please use /event %s accept|reject", evn->id);
    } else {
      buf = g_strdup_printf("Unable to create a new event!");
    }
    scr_WriteIncomingMessage(r, buf, 0, HBB_PREFIX_INFO);
    scr_LogPrint(LPRINT_LOGNORM, "%s", buf);
    g_free(buf);
  } else if (!strcmp(type, "unsubscribe")) {
    /* The sender is unsubscribing from our presence */
    jb_subscr_cancel_auth(from);
    buf = g_strdup_printf("<%s> is unsubscribing from your "
                          "presence updates", from);
    scr_WriteIncomingMessage(r, buf, 0, HBB_PREFIX_INFO);
    scr_LogPrint(LPRINT_LOGNORM, "%s", buf);
    g_free(buf);
  } else if (!strcmp(type, "subscribed")) {
    /* The sender has allowed us to receive their presence */
    buf = g_strdup_printf("<%s> has allowed you to receive their "
                          "presence updates", from);
    scr_WriteIncomingMessage(r, buf, 0, HBB_PREFIX_INFO);
    scr_LogPrint(LPRINT_LOGNORM, "%s", buf);
    g_free(buf);
  } else if (!strcmp(type, "unsubscribed")) {
    /* The subscription request has been denied or a previously-granted
       subscription has been cancelled */
    roster_unsubscribed(from);
    update_roster = TRUE;
    buf = g_strdup_printf("<%s> has cancelled your subscription to "
                          "their presence updates", from);
    scr_WriteIncomingMessage(r, buf, 0, HBB_PREFIX_INFO);
    scr_LogPrint(LPRINT_LOGNORM, "%s", buf);
    g_free(buf);
  } else {
    scr_LogPrint(LPRINT_LOGNORM, "Received unrecognized packet from <%s>, "
                 "type=%s", from, (type ? type : ""));
    newbuddy = FALSE;
  }

  if (newbuddy)
    update_roster = TRUE;
  g_free(r);
}

static void packethandler(jconn conn, jpacket packet)
{
  char *p;
  char *from=NULL, *type=NULL;

  jb_reset_keepalive(); // reset keepalive timeout
  jpacket_reset(packet);

  if (!packet->type) {
    scr_LogPrint(LPRINT_LOG, "Packet type = 0");
    return;
  }

  p = xmlnode_get_attrib(packet->x, "type");
  if (p) type = p;

  p = xmlnode_get_attrib(packet->x, "from");
  if (p) from = p;

  if (!from && packet->type != JPACKET_IQ) {
    scr_LogPrint(LPRINT_LOGNORM, "Error in stream packet");
    return;
  }

  switch (packet->type) {
    case JPACKET_MESSAGE:
        handle_packet_message(conn, type, from, packet->x);
        break;

    case JPACKET_IQ:
        handle_packet_iq(conn, type, from, packet->x);
        break;

    case JPACKET_PRESENCE:
        handle_packet_presence(conn, type, from, packet->x);
        break;

    case JPACKET_S10N:
        handle_packet_s10n(conn, type, from, packet->x);
        break;

    default:
        scr_LogPrint(LPRINT_LOG, "Unhandled packet type (%d)", packet->type);
  }
}

/* vim: set expandtab cindent cinoptions=>2\:2(0:  For Vim users... */
