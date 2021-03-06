/*
 * ebxml_receiver.c
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
#define __RECEIVER__
#endif

#ifdef __RECEIVER__

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

/***************************** receiver functions *******************/
/*
 * Some external function has opened a listen port and calls here
 * when a complete POST message has arrived.  This could be a stand-alone
 * process, or a host HTTP server.
 *
 * So in contrast to the sender, these functions only deal with buffers
 * and the network layer is found elsewhere.
 */

/*
 * An internal cache is used for duplicate detection.  At start-up we
 * might want to intialize this cache to the latest incoming.
 * The key is made up of the sender's party ID and the SOAP db Record ID.
 */
typedef struct ecache
{
  struct ebxmlcache *next;
  char *response;
  char key[1];
} ECACHE;


typedef struct
{
  MUTEX mutex;
  ECACHE *cache;
} EBXMLCACHE;

EBXMLCACHE *EbxmlCache = NULL;

/*
 * initialize
 */
void ebxml_receiver_init (XML *xml)
{
  if (EbxmlCache != NULL)
    return;
  EbxmlCache = (EBXMLCACHE *) malloc (sizeof (EBXMLCACHE));
  memset (EbxmlCache, 0, sizeof (EBXMLCACHE));
  init_mutex (EbxmlCache);
}

/*
 * return a response if found in the cache
 */
char *ebxml_duplicate (XML *soap)
{
  wait_mutex (EbxmlCache);
  end_mutex (EbxmlCache);
  return (NULL);
}


/*
 * return the map index for this service/action pair
 */
int ebxml_service_map (XML *xml, char *service, char *action, char *prefix)
{
  int i;

  debug ("getting service map for %s/%s\n", service, action);
  for (i = 0; i < xml_count (xml, "Phineas.Receiver.MapInfo.Map"); i++)
  {
    sprintf (prefix, "%s[%i].", "Phineas.Receiver.MapInfo.Map", i);
    if (strcmp (service, ebxml_get (xml, prefix, "Service")) ||
	strcmp (action, ebxml_get (xml, prefix, "Action")))
      continue;
    debug ("matched prefix %s\n", prefix);
    return (i);
  }
  *prefix = 0;
  return (-1);
}

/*
 * allocate and construct an ebXML reply message
 * caller should free the reply
 */
char *ebxml_reply (XML *xml, XML *soap, QUEUEROW *r,
  char *status, char *error, char *appdata)
{
  XML *txml;
  MIME *rmsg, *smsg, *msg;
  DBUF *b;
  char *ch,
       *organization,
       today[30],
       pid[PTIMESZ],
       buf[DBUFSZ];
  time_t t;

  time (&t);
  strcpy (today, ctime (&t));
  today[24] = 0;


  organization = xml_get_text (xml, "Phineas.Organization");
  ppid (pid);
  debug ("pid=%s\n", pid);
  /*
   * build the soap container...
   */
  txml = ebxml_template (xml, "Phineas.AckTemplate");
  // txml = ebxml_template (xml, "Phineas.SoapTemplate");
  if (txml == NULL)
  {
    error ("can't get soap template\n");
    return (NULL);
  }
  ebxml_set (txml, soap_hdr, "eb:To.eb:PartyId",
    ebxml_get (soap, soap_hdr, "eb:From.eb:PartyId"));
  ebxml_set (txml, soap_hdr, "eb:From.eb:PartyId",
    ebxml_get (soap, soap_hdr, "eb:To.eb:PartyId"));
  ebxml_set (txml, soap_hdr, "eb:CPAId",
    ebxml_get (soap, soap_hdr, "eb:CPAId"));
  ebxml_set (txml, soap_hdr, "eb:ConversationId",
    ebxml_get (soap, soap_hdr, "eb:ConversationId"));
  if (strcmp (ebxml_get (soap, soap_hdr, "eb:Action"), "Ping") == 0)
    ch = "Pong";
  else if (strcmp (error, "none"))
    ch = "MessageError";
  else
    ch = "Acknowledgment";
  ebxml_set (txml, soap_hdr, "eb:Action", ch);
  sprintf (buf, "%s@%s", pid, organization);
  ebxml_set (txml, soap_hdr, "eb:MessageData.eb:MessageId", buf);
  ebxml_set (txml, soap_hdr, "eb:MessageData.eb:Timestamp",
    ptime (NULL, buf));
  ebxml_set (txml, soap_ack, "eb:Timestamp", ptime (NULL, buf));
  queue_field_set (r, "RECEIVEDTIME", ptime (NULL, buf));
  queue_field_set (r, "LASTUPDATETIME", buf);
  ebxml_set (txml, soap_hdr, "eb:MessageData.eb:RefToMessageId",
      "statusResponse@cdc.gov");
  ebxml_set (txml, soap_hdr, "eb:MessageData.eb:RefToMessageId[1]",
    ebxml_get (soap, soap_hdr, "eb:MessageData.eb:MessageId"));
  ebxml_set (txml, soap_ack, "eb:RefToMessageId",
    ebxml_get (soap, soap_hdr, "eb:MessageData.eb:MessageId"));

  smsg = mime_alloc ();
  mime_setHeader (smsg, MIME_CONTENTID, "<ebxml-envelope@cdc.gov>", 0);
  mime_setHeader (smsg, MIME_CONTENT, MIME_XML, 1);
  mime_setBody (smsg, ch = xml_format (txml), 0);
  xml_free (txml);
  free (ch);

  /*
   * build the response container...
   * we could do this using a template, but it is trivial...
   */
  if (strcmp (ebxml_get (soap, soap_hdr, "eb:Action"), "Ping"))
  {
    b = dbuf_alloc ();
    dbuf_printf (b, "<response><msh_response><status>%s</status>"
      "<error>%s</error><appdata>%s</appdata></msh_response></response>",
      status, error, appdata);
    queue_field_set (r, "APPLICATIONSTATUS", status);
    queue_field_set (r, "ERRORCODE", error);
    queue_field_set (r, "ERRORMESSAGE", appdata);
    rmsg = mime_alloc ();
    mime_setHeader (rmsg, MIME_CONTENTID, "<statusResponse@cdc.gov>", 0);
    mime_setHeader (rmsg, MIME_CONTENT, MIME_XML, 1);
    mime_setBody (rmsg, dbuf_getbuf (b), 0);
    dbuf_free (b);
  }
  else
    rmsg = NULL;
  /*
   * package it up for transport
   */
  msg = mime_alloc ();
  mime_setBoundary (msg, "");
  mime_setHeader (msg, "SOAPAction", "\"ebXML\"", 99);
  mime_setHeader (msg, "Date", today, 99);
  mime_setHeader (msg, "Connection", "close", 99);
  mime_setHeader (msg, "Server", "PHINEAS 0.1", 99);
  mime_setMultiPart (msg, smsg);
  if (rmsg != NULL)
    mime_setMultiPart (msg, rmsg);
  ch = mime_format (msg);
  mime_free (msg);
  debug ("Reply:\n%s\n", ch);
  /*
  organization = ebxml_beautify (ch);
  debug ("Reply:\n%s\n", organization);
  free (organization);
  */
  return (ch);
}

/*
 * format up the metadata from the soap body for entry into a queue
 */
char *ebxml_format_arguments (XML *soap)
{
  XML *metadata;
  char *ch;
  char path[MAX_PATH];

  debug ("Construction metadata for ack\n");
  metadata = xml_alloc ();
  strcpy (path, soap_manifest);
  strcat (path, "MetaData");
  ch = xml_get (soap, path);
  xml_set (metadata, "Manifest.MetaData", ch);
  free (ch);
  debug ("Removing attributes from metadata\n");
  xml_clear_attributes (metadata, xml_root (metadata));
  xml_beautify (metadata, 0);
  ch = xml_format (metadata);
  xml_free (metadata);
  debug ("Returning metadata\n%s\n", ch);
  return (ch);
}

/*
 * start filling in a queue entry for an ebXML reception
 */
int ebxml_request_row (QUEUEROW *r, XML *soap)
{
  char *ch;

  queue_field_set (r, "MESSAGEID",
    ebxml_get (soap, soap_dbinf, "MessageId"));
  queue_field_set (r, "SERVICE",
    ebxml_get (soap, soap_hdr, "eb:Service"));
  queue_field_set (r, "ACTION",
    ebxml_get (soap, soap_hdr, "eb:Action"));
  queue_field_set (r, "ARGUMENTS", ch = ebxml_format_arguments (soap));
  free (ch);
  queue_field_set (r, "FROMPARTYID",
    ebxml_get (soap, soap_hdr, "eb:From.eb:PartyId"));
  queue_field_set (r, "MESSAGERECIPIENT",
    ebxml_get (soap, soap_dbinf, "MessageRecipient"));
  queue_field_set (r, "PROCESSINGSTATUS", "received");
  queue_field_set (r, "PROCESSID",
    ebxml_get (soap, soap_hdr, "eb:ConversationId"));
  return (0);
}

/*
 * Process an incoming request and return the response.  The caller
 * should free the response after sending.
 */
char *ebxml_process_req (XML *xml, char *buf)
{
  int len;			/* payload length		*/
  MIME *msg = NULL, 		/* the request message		*/
       *part = NULL;		/* one part of the message	*/
  XML *soap = NULL;		/* the ebxml soap envelope	*/
  QUEUEROW *r = NULL;		/* our audit table		*/
  FILE *fp;
  unsigned char *ch;
  char *unc,			/* decryption informatin	*/
       *pw,
       dn[DNSZ],
       prefix[MAX_PATH],	/* identifies service/action	*/
       name[MAX_PATH],		/* payload file name		*/
       path[MAX_PATH];		/* payload local disk path	*/

  info ("Begin processing ebXML request...\n");
  /*
   * Check Authorization if needed.  To keep it simple, we'll
   * use a single "realm" for all the service maps. This behavior 
   * can be modified to a per map basis, albeit with quite a bit
   * of extra work.
   */
  if (basicauth_check (xml, "Phineas.Receiver.BasicAuth", buf))
  {
    DBUF *b = basicauth_response ("Phineas Receiver");
    return (dbuf_extract (b));
  }
  debug ("request:%s\n", buf);
  if ((msg = mime_parse (buf)) == NULL)
  {
    error ("Failed to parse MIME payload\n");
    return (NULL);
  }
  if ((part = mime_getMultiPart (msg, 1)) == NULL)
  {
    error ("Failed to get SOAP envelope\n");
    mime_free (msg);
    return (NULL);
  }
  //debug ("Parsing soap part\n%s\n", mime_getBody (part));
  if ((soap = xml_parse (mime_getBody (part))) == NULL)
  {
    error ("Failed to parse SOAP xml\n");
    mime_free (msg);
    return (NULL);
  }
  /*
   * check for ping
   */
  ch = ebxml_get (soap, soap_hdr, "eb:Action");
  if (strcmp (ch, "Ping") == 0)
  {
    ch = ebxml_reply (xml, soap, NULL, "success", "none", "none");
    goto done;
  }
  /*
   * find the service map (prefix) and initialize a queue entry
   */
  if (ebxml_service_map (xml, ebxml_get (soap, soap_hdr, "eb:Service"),
    ch, prefix) < 0)
  {
    error ("Unknown service/action %s/%s\n",
	ebxml_get (soap, soap_hdr, "eb:Service"), ch);
    ch = ebxml_reply (xml, soap, NULL, "InsertFailed",
      "Unknown Service/Action", "none");
    goto done;
  }
  r = queue_row_alloc (queue_find (ch = ebxml_get (xml, prefix, "Queue")));
  if (r == NULL)
  {
    error ("queue not found for %s\n", ch);
    ch = ebxml_reply (xml, soap, NULL, "InsertFailed", 
      "Queue not found", "none");
    goto done;
  }
  ebxml_request_row (r, soap);
  /*
   * get the payload
   */
  if ((part = mime_getMultiPart (msg, 2)) == NULL)
  {
    error ("Failed to get PAYLOAD envelope\n");
    ch = ebxml_reply (xml, soap, r, "InsertFailed",
      "Missing Payload Envelope", "none");
    goto done;
  }

  /*
   * encryption envelope
   */
  unc = ebxml_get (xml, prefix, "Encryption.Unc");
  pw = ebxml_get (xml, prefix, "Encryption.Password");
  strcpy (dn, ebxml_get (xml, prefix, "Encryption.Id"));

  /*
   * get the payload, it's name, and DN
   */
  if ((len = payload_process (part, &ch, name, unc, dn, pw)) < 1)
  {
    error ("Failed processing payload - %s\n", ch);
    ch = ebxml_reply (xml, soap, r, "InsertFailed", ch, "none");
    goto done;
  }

  /*
   * prepare to write it to disk
   */
  pathf (path, "%s%s",  ebxml_get (xml, prefix, "Directory"), name);
  queue_field_set (r, "PAYLOADNAME", name);
  queue_field_set (r, "LOCALFILENAME", path);
  if (mime_getHeader (part, MIME_CONTENT) == NULL)
    queue_field_set (r, "ENCRYPTION", "no");
  else
    queue_field_set (r, "ENCRYPTION", "yes");
  /*
   * TODO save payload to disk using filter if given
   */
  info ("Writing ebXML payload to %s\n", path);
  if ((fp = fopen (path, "wb")) == NULL)
  {
    error ("Can't open %s for write\n", name);
    free (ch);
    ch = ebxml_reply (xml, soap, r, "InsertFailed",
      "Can not save file", "none");
    goto done;
  }
  fwrite (ch, 1, len, fp);
  fclose (fp);
  free (ch);

  /*
   * construct a reply and insert a queue entry
   */
  ch = ebxml_reply (xml, soap, r, "InsertSuceeded", "none", "none");

done:

  debug ("completing ebxml request processing\n");
  if (r != NULL)
  {
    queue_push (r);
    queue_row_free (r);
  }
  if (soap != NULL)
    xml_free (soap);
  if (msg != NULL)
    mime_free (msg);
  debug ("ebXML reply: %s\n", ch);
  info ("ebXML request processing completed\n");
  return (ch);
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
  int len;
  char *in, *out;

  debug ("initializing...\n");
  SSL_load_error_strings();
  SSL_library_init();
  OpenSSL_add_all_ciphers();
  xml = xml_parse (PhineasConfig);
  loadpath (xml_get_text (xml, "Phineas.InstallDirectory"));
  queue_init (xml);

  debug ("Reading test request\n");
  if ((in = readfile ("examples/request2.txt", &len)) != NULL)
  {
    out = ebxml_process_req (xml, in);
    free (in);
    free (out);
  }
  queue_shutdown ();
  xml_free (xml);
  info ("%s unit test completed\n", argv[0]);
  exit (0);
}

#endif /* UNITTEST */
#endif /* __RECEIVER__ */
