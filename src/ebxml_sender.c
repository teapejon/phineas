/*
 * ebxml_sender.c
 *
 * Copyright 2011-2012 Thomas L Dunnick
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
#define DEBUG

#ifdef UNITTEST
#define __SENDER__
#endif

#ifdef __SENDER__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef UNITTEST
#undef UNITTEST
#include "unittest.h"
#include "util.c"
#include "dbuf.c"
#include "log.c"
#include "xml.c"
#include "mime.c"
#include "queue.c"
#include "task.c"
#include "fileq.c"
#include "b64.c"
#include "basicauth.c"
#include "crypt.c"
#include "xcrypt.c"
#include "find.c"
#include "fpoller.c"
#include "qpoller.c"
#include "net.c"
#include "payload.c"
#include "ebxml.c"
#define UNITTEST
#define debug _DEBUG_
#endif

#include <stdio.h>
#include "util.h"
#include "log.h"
#include "mime.h"
#include "basicauth.h"
#include "crypt.h"
#include "xcrypt.h"
#include "payload.h"
#include "ebxml.h"


#ifndef debug
#define debug(fmt...)
#endif


/*********************** sender functions **************************/
/*
 * Receive a reply message
 */
DBUF *ebxml_receive (NETCON *conn)
{
  long n;
  int e;
  char c, *ch;
  DBUF *b;

  b = dbuf_alloc ();
  n = 0;
  /*
   * read to end of message request header - empty line
   */
  while ((e = net_read (conn, &c, 1)) == 1)
  {
    dbuf_putc (b, c);
    if (c == '\n')
    {
      if (n++ == 1)
        break;
    }
    else if (c != '\r')
      n = 0;
  }
  if (e != 1)
  {
    if (e == 0)
      error ("Acknowledgment header read failed or connection closed\n");
    else
      error ("Timed out reading acknowledgment header\n");
    dbuf_free (b);
    return (NULL);
  }
  ch = strnstr (dbuf_getbuf (b), "Content-Length: ", dbuf_size (b));
  if (ch == NULL)
  {
    error ("Acknowledgment header missing Content-Length\n");
    dbuf_free (b);
    return (NULL);
  }
  n = atol (ch + 16);
  debug ("expecting %d bytes\n", n);
  while (n--)
  {
    if ((e = net_read (conn, &c, 1)) != 1)
    {
      if (e == 0)
        error ("Acknowledgment content read failed or connection closed");
      else
        error ("Timed out reading acknowledgment content\n");
      // note we'll take what we get and hope it's enough...
      break;
    }
    dbuf_putc (b, c);
  }
  debug ("returning %d bytes\n", dbuf_size (b));
  return (b);
}

/*
 * Get the MESSAGEID from a queue row and use it to set the
 * appropriate xml Map path entry and pid for the message.
 * The MESSAGEID should be of the form Map.Name-PID.
 *
 * prefix is a buffer big enough to hold the xml path to the matching
 * Map entry AND the pid which is appended.  The pointer to the pid
 * is returned if successful.
 */

char *ebxml_pid (XML *xml, QUEUEROW *r, char *prefix)
{
  int i, n;
  char *id, *pid, *ch;

  id = queue_field_get (r, "MESSAGEID");
  if ((pid = strchr (id, '-')) == NULL)
    return (NULL);

  n = xml_count (xml, "Phineas.Sender.MapInfo.Map");
  for (i = 0; i < n; i++)
  {
    sprintf (prefix, "Phineas.Sender.MapInfo.Map[%d].", i);
    if (strncmp (ebxml_get (xml, prefix, "Name"), id, pid - id) == 0)
    {
      id = prefix + strlen (prefix) + 2;
      strcpy (id, pid);
      return (id);
    }
  }
  return (NULL);
}

/*
 * Get the route index for a name
 */
int ebxml_route_index (XML *xml, char *name)
{
  int i, n;

  n = xml_count (xml, "Phineas.Sender.RouteInfo.Route");
  for (i = 0; i < n; i++)
  {
    if (strcmp (name,
	  xml_getf (xml, "Phineas.Sender.RouteInfo.Route[%d].Name", i)) == 0)
      return (i);
  }
  error ("No route found for %s\n", name);
  return (-1);
}

/*
 * Get route data for index and tag
 */
char *ebxml_route_info (XML *xml, int index, char *tag)
{
  if (index < 0)
    return ("");
  return (xml_getf (xml, "Phineas.Sender.RouteInfo.Route[%d].%s", index, tag));
}

/*
 * Build the mime payload container
 */
MIME *ebxml_getpayload (XML *xml, QUEUEROW *r)
{
  int l;
  XML *exml;
  MIME *msg;
  char *pid,
       *b,			/* buffer for payload		*/
       *type,
       *unc = NULL,		/* encryption info		*/
       *pw = NULL,
       dn[DNSZ],
       *organization,
       prefix[MAX_PATH],
       buf[MAX_PATH],
       fname[MAX_PATH];

  debug ("getpayload container...\n");
  organization = xml_get_text (xml, "Phineas.Organization");
  pid = ebxml_pid (xml, r, prefix);
  pathf (fname, "%s%s", ebxml_get (xml, prefix, "Processed"),
    queue_field_get (r, "PAYLOADFILE"));
  // TODO invoke filter if specified
  debug ("reading data from %s\n", fname);
  if ((b = readfile (fname, &l)) == NULL)
  {
    error ("Can't read %s - %s\n", fname, strerror (errno));
    return (NULL);
  }

  organization = xml_get_text (xml, "Phineas.Organization");
  type = ebxml_get (xml, prefix, "Encryption.Type");
  if ((type != NULL) && *type)	/* encrypted			*/
  {
    unc = ebxml_get (xml, prefix, "Encryption.Unc");
    pw = ebxml_get (xml, prefix, "Encryption.Password");
    strcpy (dn, ebxml_get (xml, prefix, "Encryption.Id"));
  }
  msg = payload_create (b, l, fname, organization, unc, dn, pw);
  free (b);
  if (msg == NULL)
    error ("Can't create payload container for %s\n", fname);
  return (msg);
}

/*
 * Get the mime header (soap) container
 */
MIME *ebxml_getsoap (XML *xml, QUEUEROW *r)
{
  XML *soap;
  MIME *msg;
  char *ch,
       *pid,
       *partyid,
       *cpa,
       *organization,
       path[MAX_PATH],
       buf[MAX_PATH];
  int route;

  debug ("getting soap container...\n");
  organization = xml_get_text (xml, "Phineas.Organization");
  pid = queue_field_get (r, "MESSAGEID");
  if (pid != NULL)
    pid = strchr (pid, '-');
  if (pid++ == NULL)
  {
    error ("Can't get PID from MESSAGEID\n");
    return (NULL);
  }
  route = ebxml_route_index (xml, queue_field_get (r, "ROUTEINFO"));
  if (route < 0)
    return (NULL);

  debug ("getting soap template for pid=%s org=%s...\n", pid, organization);
  if ((soap = ebxml_template (xml, "Phineas.SoapTemplate")) == NULL)
  {
    error ("Can't get SOAP template\n");
    return (NULL);
  }
  ebxml_set (soap, soap_hdr, "eb:From.eb:PartyId",
    xml_get_text (xml, "Phineas.PartyId"));
  partyid = "Someone_else";
  ebxml_set (soap, soap_hdr, "eb:To.eb:PartyId", partyid);
  cpa = ebxml_route_info (xml, route, "Cpa");
  ebxml_set (soap, soap_hdr, "eb:CPAId", cpa);
  ebxml_set (soap, soap_hdr, "eb:ConversationId", pid);
  ebxml_set (soap, soap_hdr, "eb:Service", queue_field_get (r, "SERVICE"));
  ebxml_set (soap, soap_hdr, "eb:Action", queue_field_get (r, "ACTION"));
  sprintf (buf, "%ld@%s", pid, organization);
  ebxml_set (soap, soap_hdr, "eb:MessageData.eb:MessageId", buf);
  queue_field_set (r, "MESSAGECREATIONTIME", ptime (NULL, buf));
  ebxml_set (soap, soap_hdr, "eb:MessageData.eb:Timestamp", buf);
  if (!strcmp (queue_field_get (r, "ACTION"), "Ping"))
  {
    xml_delete (soap, soap_bdy);
  }
  else
  {
    sprintf (path, "%s%s", soap_manifest, "eb:Reference");
    sprintf (buf, "cid:%s@%s", queue_field_get (r, "PAYLOADFILE"),
      organization);
    debug ("set path=%s val=%s\n", path, buf);
    xml_set_attribute (soap, path, "xlink:href", buf);
    sprintf (buf, "%s.%s", r->queue->name, queue_field_get (r, "RECORDID"));
    ebxml_set (soap, soap_dbinf, "RecordId", buf);
    ebxml_set (soap, soap_dbinf, "MessageId",
      queue_field_get (r, "MESSAGEID"));
    ebxml_set (soap, soap_dbinf, "Arguments",
      queue_field_get (r, "ARGUMENTS"));
    ebxml_set (soap, soap_dbinf, "MessageRecipient",
      queue_field_get (r, "MESSAGERECIPIENT"));
  }
  debug ("building soap mime container...\n");
  msg = mime_alloc ();
  mime_setHeader (msg, MIME_CONTENT, MIME_XML, 99);
  sprintf (buf, "<%s%s>", "ebxml-envelope@", organization);
  mime_setHeader (msg, MIME_CONTENTID, buf, 0);
  debug ("formatting soap xml...\n");
  ch = xml_format (soap);
  xml_free (soap);
  mime_setBody (msg, ch, strlen (ch));
  free (ch);
  debug ("returning soap message...\n");
  return (msg);
}

/*
 * Get the mime ebXML message
 */
MIME *ebxml_getmessage (XML *xml, QUEUEROW *r)
{
  MIME *msg, *payload, *soap;
  char *pid,
       *organization,
       buf[MAX_PATH];
  int route;

  route = ebxml_route_index (xml, queue_field_get (r, "ROUTEINFO"));
  if (route < 0)
    return (NULL);
  organization = xml_get_text (xml, "Phineas.Organization");
  pid = queue_field_get (r, "MESSAGEID");
  if (pid != NULL)
    pid = strchr (pid, '-');
  if (pid++ == NULL)
  {
    error ("Can't get PID from MESSAGEID\n");
    return (NULL);
  }
  if (strcmp (queue_field_get (r, "ACTION"), "Ping"))
  {
    if ((payload = ebxml_getpayload (xml, r)) == NULL)
      return (NULL);
  }
  else
    payload = NULL;
  if ((soap = ebxml_getsoap (xml, r)) == NULL)
  {
    mime_free (payload);
    return (NULL);
  }
  debug ("building multipart mime message\n");
  msg = mime_alloc ();
  sprintf (buf, "type=\"text/xml\"; start=\"ebxml-envelope@%s\";",
     organization);
  mime_setBoundary (msg, buf);
  sprintf (buf, "%s:%s",
    ebxml_route_info (xml, route, "Host"),
    ebxml_route_info (xml, route, "Port"));
  mime_setHeader (msg, "Host", buf, 99);
  mime_setHeader (msg, "Connection", "Close", 99);
  if (strcmp ("basic", 
    ebxml_route_info (xml, route, "Authentication.Type")) == 0)
  {
    basicauth_request (buf, 
      ebxml_route_info (xml, route, "Authentication.Id"),
      ebxml_route_info (xml, route, "Authentication.Password"));
    pid = strchr (buf, ':');
    *pid++ = 0;
    while (isspace (*pid)) pid++;
    mime_setHeader (msg, buf, pid, 99);
  }
  mime_setHeader (msg, "SOAPAction", "\"ebXML\"", 99);
  mime_setMultiPart (msg, soap);
  if (payload != NULL)
    mime_setMultiPart (msg, payload);
  debug ("Completed multipart soap message\n");
  return (msg);
}

/*
 * queue a Ping request for this route
 */
int ebxml_qping (XML *xml, int route)
{
  QUEUE *q;
  QUEUEROW *r;
  int pl;
  char *ch,
  buf[MAX_PATH],
  pid[PTIMESZ];

  /*
   * queue it up
   */
  ch = ebxml_route_info (xml, route, "Queue");
  debug ("queuing ping for %s\n", ch);
  if ((q = queue_find (ch)) == NULL)
  {
    error ("Can't find queue for %s\n", ch);
    return (-1);
  }
  ppid (pid);
  debug ("prepping new row\n");
  r = queue_row_alloc (q);
  sprintf (buf, "%s-%s", ebxml_route_info (xml, route, "Name"), pid);
  queue_field_set (r, "MESSAGEID", buf);
  queue_field_set (r, "PAYLOADFILE", "");
  queue_field_set (r, "DESTINATIONFILENAME", "");
  queue_field_set (r, "ROUTEINFO", ebxml_route_info (xml, route, "Name"));
  queue_field_set (r, "SERVICE", "urn:oasis:names:tc:ebxml-msg:service");
  queue_field_set (r, "ACTION", "Ping");
  queue_field_set (r, "ARGUMENTS", 
    ebxml_route_info (xml, route, "Arguments"));
  queue_field_set (r, "MESSAGERECIPIENT",
  ebxml_route_info (xml, route, "Recipient"));
  queue_field_set (r, "ENCRYPTION","no");
  queue_field_set (r, "SIGNATURE", "no");
  queue_field_set (r, "PUBLICKEYLDAPADDRESS", "");
  queue_field_set (r, "PUBLICKEYLDAPBASEDN", "");
  queue_field_set (r, "PUBLICKEYLDAPDN", "");
  queue_field_set (r, "CERTIFICATEURL","");
  queue_field_set (r, "PROCESSINGSTATUS", "queued");
  queue_field_set (r, "TRANSPORTSTATUS", "");
  queue_field_set (r, "PRIORITY", "0");
  debug ("pushing the queue\n");
  if (pl = queue_push (r) < 1)
  {
    error ("Failed queueing ping for %s\n", 
	ebxml_route_info (xml, route, "Name"));
    pl = -1;
  }
  queue_row_free (r);
  info ("ebXML Ping for %s queueing completed\n",
    ebxml_route_info (xml, route, "Name"));
  return (0);
}


/*
 * A folder polling processor for ebxml queues - register this with
 * the fpoller.
 *
 * This initializes and pushes a queue row.  Once queued it moves
 * the file to a processed point.
 *
 * xml - sender's configuration
 * prefix - xml path to this folder map
 * fname - file to be queued
 */
int ebxml_fprocessor (XML *xml, char *prefix, char *fname)
{
  struct stat st;
  QUEUE *q;
  QUEUEROW *r;
  char *ch;
  int pl;
  char qname[MAX_PATH],
       pid[PTIMESZ],
       buf[MAX_PATH];

  if (stat (fname, &st))
  {
    warn ("Can't access %s\n", fname);
    return (-1);
  }
  if (st.st_size == 0)
  {
    warn ("File %s empty... discarding\n", fname);
    unlink (fname);
    return (-1);
  }
  info ("Queuing ebXML folder %s for %s\n", fname, prefix);
  pl = strlen (prefix);
  /*
   * prep a file name
   */
  ppid (pid);
  sprintf (qname, "%s.%s", basename (fname), pid);

  /*
   * move file to processed folder
   */
  pathf (buf, "%s%s", ebxml_get (xml, prefix, "Processed"), qname);
  if (rename (fname, buf))
  {
    error ("Couldn't move %s to %s\n", fname, buf);
    return (-1);
  }

  /*
   * queue it up
   */
  ch = ebxml_get (xml, prefix, "Queue");
  if ((q = queue_find (ch)) == NULL)
  {
    error ("Can't find queue for %s\n", ch);
    return (-1);
  }
  r = queue_row_alloc (q);
  sprintf (buf, "%s-%s", ebxml_get (xml, prefix, "Name"), pid);
  queue_field_set (r, "MESSAGEID", buf);
  queue_field_set (r, "PAYLOADFILE", qname);
  queue_field_set (r, "DESTINATIONFILENAME", basename (fname));
  queue_field_set (r, "ROUTEINFO", ebxml_get (xml, prefix, "Route"));
  queue_field_set (r, "SERVICE", ebxml_get (xml, prefix, "Service"));
  queue_field_set (r, "ACTION", ebxml_get (xml, prefix, "Action"));
  queue_field_set (r, "ARGUMENTS", ebxml_get (xml, prefix, "Arguments"));
  queue_field_set (r, "MESSAGERECIPIENT",
    ebxml_get (xml, prefix, "Recipient"));
  queue_field_set (r, "ENCRYPTION",
    *ebxml_get (xml, prefix, "Encryption.Type") ? "yes" : "no");
  queue_field_set (r, "SIGNATURE", "no");
  queue_field_set (r, "PUBLICKEYLDAPADDRESS", "");
  queue_field_set (r, "PUBLICKEYLDAPBASEDN", "");
  queue_field_set (r, "PUBLICKEYLDAPDN", "");
  queue_field_set (r, "CERTIFICATEURL",
    ebxml_get (xml, prefix, "Encryption.Unc"));
  queue_field_set (r, "PROCESSINGSTATUS", "queued");
  queue_field_set (r, "TRANSPORTSTATUS", "");
  queue_field_set (r, "PRIORITY", "0");
  if (pl = queue_push (r) < 1)
  {
    error ("Failed queueing %s\n", fname);
    pl = -1;
  }
  queue_row_free (r);
  info ("ebXML folder %s for %s queueing completed\n", fname, prefix);
  return (pl);
}

/*
 * get an SSL context for this route
 */
SSL_CTX *ebxml_route_ctx (XML *xml, int route)
{
  char *id, *passwd, *unc, *ca,
       pathbuf[MAX_PATH * 2];

  debug ("getting SSL context for route %d\n", route);
  id = ebxml_route_info (xml, route, "Protocol");
  if (stricmp (id, "https"))
    return (NULL);
  id = ebxml_route_info (xml, route, "Authentication.Type");
  debug ("authentication type %s\n", id);
  if (strcmp ("certificate", id) == 0)
  {
    id = ebxml_route_info (xml, route, "Authentication.Id");
    passwd = ebxml_route_info (xml, route, "Authentication.Password");
    unc = pathf (pathbuf, "%s",
      ebxml_route_info (xml, route, "Authentication.Unc"));
    debug ("unc path=%s\n", unc);
  }
  else 
  {
    if (*id)
    {
      error ("Authentication Type %s not supported\n", id);
      return (NULL);
    }
    id = NULL;
    passwd = NULL;
    unc = NULL;
  }
  if (*(ca = xml_get_text (xml, "Phineas.Sender.CertificateAuthority")))
    ca = pathf (pathbuf + MAX_PATH, "%s", ca);
  debug ("ca path=%s\n", ca);
  return (net_ctx (unc, unc, passwd, ca, 0));
}

/*
 * check the http status code for problems
 */
int ebxml_status (char *reply)
{
  char *ch, *nl;

  if ((nl = strchr (reply, '\n')) == NULL)
    nl = reply + strlen (reply);
  if (((ch = strchr (reply, ' ')) != NULL) && (ch < nl))
  {
    while (isspace (*ch)) ch++;
    if (atoi (ch) == 200)
      return (0);
  }
  else
    ch = reply;
  error ("EbXML reply failed: %.*s\n", nl - ch, ch);
  return (-1);
}

/*
 * parse a reply message and update the queue row with status
 */
int ebxml_parse_reply (char *reply, QUEUEROW *r)
{
  MIME *msg, *part;
  XML *rxml;
  char buf[PTIMESZ];

  if (ebxml_status (reply))
    return (-1);
  if ((msg = mime_parse (reply)) == NULL)
  {
    error ("Failed parsing reply message\n");
    return (-1);
  }
  /* Ping reply */
  if (strcmp (queue_field_get (r, "ACTION"), "Ping") == 0)
  {
    if ((part = mime_getMultiPart (msg, 1)) == NULL)
    {
      error ("Reply missing status part\n");
      mime_free (msg);
      return (-1);
    }
    if (strstr (mime_getBody (part), "Pong") == NULL)
    {
      error ("Expected 'Pong' action\n");
      mime_free (msg);
      return (-1);
    }
    queue_field_set (r, "APPLICATIONSTATUS", "not-set");
    queue_field_set (r, "APPLICATIONERRORCODE", "none");
    queue_field_set (r, "APPLICATIONRESPONSE", "none");
    queue_field_set (r, "MESSAGERECEIVEDTIME", ptime (NULL, buf));
  }
  else /* regular reply */
  {
    if ((part = mime_getMultiPart (msg, 2)) == NULL)
    {
      error ("Reply missing status part\n");
      mime_free (msg);
      return (-1);
    }
    debug ("Body is...\n%s\n", mime_getBody (part));
    if ((rxml = xml_parse (mime_getBody (part))) == NULL)
    {
      error ("Miss formatted Reply status\n");
      mime_free (msg);
      return (-1);
    }
    queue_field_set (r, "APPLICATIONSTATUS",
      xml_get_text (rxml, "response.msh_response.status"));
    queue_field_set (r, "APPLICATIONERRORCODE",
      xml_get_text (rxml, "response.msh_response.error"));
    queue_field_set (r, "APPLICATIONRESPONSE",
      xml_get_text (rxml, "response.msh_response.appdata"));
    queue_field_set (r, "MESSAGERECEIVEDTIME", ptime (NULL, buf));
  
    /* TODO...
    queue_field_set (r, "RESPONSEMESSAGEID", "");
    queue_field_set (r, "RESPONSEARGUMENTS", "");
    queue_field_set (r, "RESPONSELOCALFILE", "");
    queue_field_set (r, "RESPONSEFILENAME", "");
    queue_field_set (r, "RESPONSEMESSAGEORIGIN", "");
    queue_field_set (r, "RESPONSEMESSAGESIGNATURE", "");
     */
    xml_free (rxml);
  }
  queue_field_set (r, "PROCESSINGSTATUS", "done");
  queue_field_set (r, "TRANSPORTSTATUS", "success");
  queue_field_set (r, "TRANSPORTERRORCODE", "none");

  mime_free (msg);
  return (0);
}

/*
 * check a reply for a redirect, and if found set the new host, port,
 * and path to the redirected URL
 */
int ebxml_redirect (char *reply, char *host, int *port, char *path)
{
  char *ch;
  int v;

  /* get the response code 300-399 are redirects		*/
  if ((ch = strchr (reply, ' ')) == NULL)
    return (0);
  v = atoi (ch);
  if ((v < 300) || (v > 399))
    return (0);
  /* the location specifies a new URL				*/
  if ((ch = strstr (reply, "Location:")) == NULL)
    return (0);
  /* assume port 80 and parse the URL				*/
  ch += 10;
  *port = 80;
  if (!strncmp (ch, "https:", 6))
  {
    ch++;
    *port = 443;
  }
  /* parse out the host and optional port			*/
  ch += 7;
  while ((*host = *ch++) != '/')
  {
    if (*host == ':')
    {
      *port = atoi (ch);
      while (*++ch != '/');
      break;
    }
    host++;
  }
  *host = 0;
  /* finally parse out the path					*/
  while ((*path = *ch++) >= ' ') path++;
  *path = 0;
  return (1);
}

/*
 * send a message
 */
int ebxml_send (XML*xml, QUEUEROW *r, MIME *msg)
{
  DBUF *b;
  NETCON *conn;
  char host[MAX_PATH];
  char path[MAX_PATH];
  int port, route;
  SSL_CTX *ctx;
  DBUF *b;
  char *content, buf[MAX_PATH];

  /* format up the message					*/
  if ((content = mime_format (msg)) == NULL)
  {
    queue_field_set (r, "PROCESSINGSTATUS", "done");
    queue_field_set (r, "TRANSPORTSTATUS", "failed");
    queue_field_set (r, "TRANSPORTERRORCODE", "failed formatting message");
    return (0);
  }

  /*
   * get connection info from the record route
   */
  route = ebxml_route_index (xml, queue_field_get (r, "ROUTEINFO"));
  ctx = ebxml_route_ctx (xml, route);
  strcpy (host, ebxml_route_info (xml, route, "Host"));
  port = atoi (ebxml_route_info (xml, route, "Port"));
  strcpy (path, ebxml_route_info (xml, route, "Path"));

sendmsg:

  debug ("opening connection socket on port %d\n", port);
  if ((conn = net_open (host, port, 0, ctx)) == NULL)
  {
    error ("failed opening connection to %s:%d\n", host, port);
    if (ctx != NULL)
      SSL_CTX_free (ctx);
    free (content);
    return (-1);
  }
  queue_field_set (r, "MESSAGESENTTIME", ptime (NULL, buf));
  sprintf (buf, "POST %s HTTP/1.1\r\n", path);
  // ch = ebxml_beautify (ch);
  debug ("sending message...\n");
  net_write (conn, buf, strlen (buf));
  net_write (conn, content, strlen (content));
  debug ("reading response...\n");
  b = ebxml_receive (conn);
  debug ("closing socket...\n");
  net_close (conn);
  if (b == NULL)
  {
    if (ctx != NULL)
      SSL_CTX_free (ctx);
    free (content);
    return (-1);
  }
  debug ("reply was %d bytes\n%.*s\n", dbuf_size (b),
    dbuf_size (b), dbuf_getbuf (b));

  /*
   * handle redirects...
   * note this assumes the same SSL context should be used
   */
  if (ebxml_redirect (dbuf_getbuf (b), host, &port, path))
  {
    dbuf_free (b);
    goto sendmsg;
  }

  if (ctx != NULL)
    SSL_CTX_free (ctx);
  if (ebxml_parse_reply (dbuf_getbuf (b), r))
  {
    queue_field_set (r, "PROCESSINGSTATUS", "done");
    queue_field_set (r, "TRANSPORTSTATUS", "failed");
    queue_field_set (r, "TRANSPORTERRORCODE", "garbled reply");
  }
  dbuf_free (b);
  free (content);
  return (0);
}

int ebxml_record_ack (XML *xml, QUEUEROW *r)
{
  FILE *fp;
  char *ch,
       prefix[MAX_PATH],
       fname[MAX_PATH];

  ebxml_pid (xml, r, prefix);
  ch = ebxml_get (xml, prefix, "Acknowledged");
  if (*ch == 0)
    return (0);
  pathf (fname, "%s%s", ch, queue_field_get (r, "PAYLOADFILE"));
  if ((fp = fopen (fname, "w")) == NULL)
    return (0);
  fprintf (fp,
    "transportStatus=%s\n"
    "transportError=%s\n"
    "applicationStatus=%s\n"
    "applicationError=%s\n"
    "applicationData=%s\n"
    "responseMessageId=%s\n"
    "responseArguments=%s\n"
    "responseLocalFile=%s\n"
    "responseFileName=%s\n"
    "responseSignature=%s\n"
    "responseMessageOrigin=%s\n",
    queue_field_get (r, "TRANSPORTSTATUS"),
    queue_field_get (r, "TRANSPORTERRORCODE"),
    queue_field_get (r, "APPLICATIONSTATUS"),
    queue_field_get (r, "APPLICATIONERRORCODE"),
    queue_field_get (r, "APPLICATIONRESPONSE"),
    queue_field_get (r, "RESPONSEMESSAGEID"),
    queue_field_get (r, "RESPONSEARGUMENTS"),
    queue_field_get (r, "RESPONSELOCALFILE"),
    queue_field_get (r, "RESPONSEFILENAME"),
    queue_field_get (r, "RESPONSEMESSAGESIGNATURE"),
    queue_field_get (r, "RESPONSEMESSAGEORIGIN"));
  fclose (fp);
  return (0);
}

/*
 * A queue polling processor for ebxml queues - register this with
 * the qpoller.
 *
 * This builds an ebXML MIME message, opens a connection to a
 * receiver, sends the request, processes the response, and
 * finally updates the queue status.
 */
int ebxml_qprocessor (XML *xml, QUEUEROW *r)
{
  MIME *m;
  int try, retries, delay;

  info ("Send ebXML queue %s row %d\n", r->queue->name, r->rowid);
  /*
   * build an ebXML MIME message
   */
  if ((m = ebxml_getmessage (xml, r)) == NULL)
  {
    char buf[24];
    queue_field_set (r, "MESSAGECREATIONTIME", ptime (NULL, buf));
    queue_field_set (r, "PROCESSINGSTATUS", "done");
    queue_field_set (r, "TRANSPORTSTATUS", "failed");
    queue_field_set (r, "TRANSPORTERRORCODE", "bad message");
    queue_push (r);
    return (-1);
  }
  /*
   * update the queue with message status
   */
  debug ("updating queue\n");
  queue_field_set (r, "PROCESSINGSTATUS", "waiting");
  queue_field_set (r, "TRANSPORTSTATUS", "attempted");
  queue_push (r);
  /*
   * send it to the destination
   */
  debug ("sending to destination\n");
  if ((retries = xml_get_int (xml, "Phineas.Sender.MaxRetry")) == 0)
    retries = 1;
  if ((delay = xml_get_int (xml, "Phineas.Sender.DelayRetry")) == 0)
    delay = 5;
  for (try = 0; try < retries; try++)
  {
    debug ("try=%d\n", try);
    if (ebxml_send (xml, r, m) == 0)
      break;
    sleep (delay * 1000);
    if (delay < 3600)
      delay <<= 1;
  }
  if (try >= retries)
  {
    queue_field_set (r, "PROCESSINGSTATUS", "done");
    queue_field_set (r, "TRANSPORTSTATUS", "failed");
    queue_field_set (r, "TRANSPORTERRORCODE", "retries exhausted");
  }
  debug ("getting ACK\n");
  ebxml_record_ack (xml, r);
  /*
   * update the queue with status from the reply
   */
  debug ("updating reply\n");
  queue_push (r);
  /*
   * release all memory
   */
  mime_free (m);
  info ("ebXML queue %s row %d send completed\n", r->queue->name, r->rowid);
  return (0);
}

#ifdef UNITTEST

int ran = 0;
int phineas_running  ()
{
  return (ran++ < 3);
}

int main (int argc, char **argv)
{
  XML *xml;
  QUEUE *q;
  QUEUEROW *r;
  MIME *m;
  char *ch;
  int arg = 1;

  xml = xml_parse (PhineasConfig);
  loadpath (xml_get_text (xml, "Phineas.InstallDirectory"));
  queue_init (xml);

  /* test folder polling */
  if ((argc > arg) && (strcmp (argv[arg], "-f") == 0))
  {
    fpoller_register ("ebxml", ebxml_fprocessor);
    fpoller_task (xml);
    arg++;
    ran = 0;
  }
  /* test message from queue */
  if ((argc > arg) && (strcmp (argv[arg], "-m") == 0))
  {
    if ((q = queue_find ("MemSendQ")) == NULL)
      error ("can't find MemSendQ");
    else if ((r = queue_pop (q)) == NULL)
      error ("can't pop row\n");
    /*
    else if ((m = ebxml_getsoap (xml, r)) == NULL)
      error ("can't get soap container\n");
    */
    else if ((m = ebxml_getmessage (xml, r)) == NULL)
      error ("can't get message\n");
    else if ((ch = mime_format (m)) == NULL)
      error ("Can't format soap containter\n");
    else
      debug ("message MIME\n%s\n", ch);
    free (ch);
    mime_free (m);
    queue_row_free (r);
    arg++;
  }
  /* test queue polling */
  if ((argc > arg) && (strcmp (argv[arg], "-q") == 0))
  {
    qpoller_register ("EbXmlSndQ", ebxml_qprocessor);
    qpoller_task (xml);
    arg++;
    ran = 0;
  }
  queue_shutdown ();
  xml_free (xml);
  info ("%s unit test completed\n", argv[0]);
  exit (0);
}

#endif /* UNITTEST */
#endif /* __SENDER__ */
