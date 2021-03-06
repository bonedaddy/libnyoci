/*	@file nyoci-var-handler.c
**	@author Robert Quattlebaum <darco@deepdarc.com>
**
**	Copyright (C) 2017  Robert Quattlebaum
**
**	Permission is hereby granted, free of charge, to any person
**	obtaining a copy of this software and associated
**	documentation files (the "Software"), to deal in the
**	Software without restriction, including without limitation
**	the rights to use, copy, modify, merge, publish, distribute,
**	sublicense, and/or sell copies of the Software, and to
**	permit persons to whom the Software is furnished to do so,
**	subject to the following conditions:
**
**	The above copyright notice and this permission notice shall
**	be included in all copies or substantial portions of the
**	Software.
**
**	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
**	KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
**	WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
**	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
**	OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
**	OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
**	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
**	SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#if HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef VERBOSE_DEBUG
#define VERBOSE_DEBUG 0
#endif

#include "assert-macros.h"

#include "libnyoci.h"
#include "nyoci-internal.h"
#include "nyoci-missing.h"
#include "nyoci-var-handler.h"
#include "nyoci-logging.h"
#include "fasthash.h"

#include "nyoci-missing.h" // For strhasprefix_const()

#include "url-helpers.h"
#include <stdlib.h>

#define BAD_KEY_INDEX		(255)

nyoci_status_t
nyoci_var_handler_request_handler(
	nyoci_var_handler_t		node
) {
	// TODO: Make this function use less stack space!

	coap_code_t method = nyoci_inbound_get_code();
	nyoci_status_t ret = NYOCI_STATUS_NOT_FOUND;
	NYOCI_NON_RECURSIVE coap_content_type_t content_type;
	NYOCI_NON_RECURSIVE char* content_ptr;
	NYOCI_NON_RECURSIVE coap_size_t content_len;
	NYOCI_NON_RECURSIVE char buffer[NYOCI_VARIABLE_MAX_VALUE_LENGTH+1];
	NYOCI_NON_RECURSIVE coap_content_type_t reply_content_type;
	uint8_t key_index = BAD_KEY_INDEX;
	coap_size_t value_len;
	bool needs_prefix = true;
	char* prefix_name = "";

	content_type = nyoci_inbound_get_content_type();
	content_ptr = (char*)nyoci_inbound_get_content_ptr();
	content_len = nyoci_inbound_get_content_len();

	reply_content_type = NYOCI_CONTENT_TYPE_APPLICATION_FORM_URLENCODED;

	require(node, bail);

	// Look up the key index.
	if (nyoci_inbound_peek_option(NULL,&value_len)==COAP_OPTION_URI_PATH) {
		if(0 == value_len) {
			// Trailing Slash, we will return the directory contents.
			needs_prefix = false;
			nyoci_inbound_next_option(NULL,NULL);

		} else {
			// Find the index associated with this key.
			for (key_index = 0; key_index < BAD_KEY_INDEX; key_index++) {
				ret = node->func(node,NYOCI_VAR_GET_KEY,key_index,buffer);

				require_action(ret == 0, bail, ret = NYOCI_STATUS_NOT_FOUND);

				if(nyoci_inbound_option_strequal(COAP_OPTION_URI_PATH, buffer)) {
					nyoci_inbound_next_option(NULL,NULL);
					break;
				}
			}
		}
	} else {
		// This is the case where we are not actually
		// in the path of our directory. (The slash is missing)

		// We were previously not handling this case
		// properly. Better to fail hard than return bad data.
		ret = nyoci_outbound_quick_response(COAP_RESULT_406_NOT_ACCEPTABLE, "Needs trailing slash");
		goto bail;
	}

	{
		coap_option_key_t key;
		const uint8_t* value;
		while((key=nyoci_inbound_next_option(&value, &value_len))!=COAP_OPTION_INVALID) {
			require_action(key!=COAP_OPTION_URI_PATH,bail,ret=NYOCI_STATUS_NOT_FOUND);
			if (key == COAP_OPTION_URI_QUERY) {
				if(	method == COAP_METHOD_POST
					&& value_len>=2
					&& strhasprefix_const((const char*)value,"v=")
				) {
					DEBUG_PRINTF("variable-node: value is in the query.");
					content_type = COAP_CONTENT_TYPE_TEXT_PLAIN;
					content_ptr = (char*)value+2;
					content_len = value_len-2;
				}
//			} else if(key==COAP_OPTION_ETAG) {
//			} else if(key==COAP_OPTION_IF_MATCH) {
//			} else if(key==COAP_OPTION_IF_NONE_MATCH) {

			} else if(key==COAP_OPTION_ACCEPT) {
				reply_content_type = (coap_content_type_t)coap_decode_uint32(value, (uint8_t)value_len);

			} else if(COAP_OPTION_IS_CRITICAL(key)) {
				ret = NYOCI_STATUS_BAD_OPTION;
				assert_printf("Unrecognized option %d, \"%s\"",
					key,
					coap_option_key_to_cstr(key, false)
				);
				goto bail;
			}
		}
	}

	// TODO: Implement me!
	if (method == COAP_METHOD_PUT) {
		method = COAP_METHOD_POST;
	}

	if (method == COAP_METHOD_POST) {
		require_action(
			key_index != BAD_KEY_INDEX,
			bail,
			ret = NYOCI_STATUS_NOT_ALLOWED
		);

		if (!nyoci_inbound_is_dupe()) {
			if (content_type == NYOCI_CONTENT_TYPE_APPLICATION_FORM_URLENCODED) {
				char* key = NULL;
				char* value = NULL;
				content_len = 0;
				while(
					url_form_next_value(
						(char**)&content_ptr,
						&key,
						&value
					)
					&& key
					&& value
				) {
					if(strequal_const(key, "v")) {
						content_ptr = value;
						content_len = (coap_size_t)strlen(value);
						break;
					}
				}
			}

			// Make sure our content is zero terminated.
			((char*)content_ptr)[content_len] = 0;

			ret = node->func(node, NYOCI_VAR_SET_VALUE, key_index, (char*)content_ptr);
			require_noerr(ret, bail);
		}

		// Don't send responses to multicast posts or puts
		if (nyoci_inbound_is_multicast()) {
			goto bail;
		}

		ret = nyoci_outbound_begin_response(COAP_RESULT_204_CHANGED);
		require_noerr(ret, bail);

		ret = nyoci_outbound_send();
		require_noerr(ret, bail);

	} else if (method == COAP_METHOD_GET) {

		if (key_index == BAD_KEY_INDEX) {
			char* content_end_ptr;
			uint32_t max_age = UINT32_MAX;
			bool observable = false;

			ret = nyoci_outbound_begin_response(COAP_RESULT_205_CONTENT);
			require_noerr(ret, bail);

			// Calculate our max age and if we support observing
			for (key_index=0; key_index < BAD_KEY_INDEX; key_index++) {
				if ( !observable
				  && NYOCI_STATUS_OK == node->func(node,NYOCI_VAR_GET_OBSERVABLE,key_index,NULL)
				) {
					observable = true;
				}
				if (NYOCI_STATUS_OK == node->func(node,NYOCI_VAR_GET_MAX_AGE,key_index,buffer)) {
#if HAVE_STRTOL
					uint32_t tmp = strtol(buffer,NULL,0) & 0xFFFFFF;
#else
					uint32_t tmp = atoi(buffer) & 0xFFFFFF;
#endif
					if (tmp < max_age) {
						max_age = tmp;
					}
				}
			}

			if (observable) {
				ret = nyoci_observable_update(&node->observable, NYOCI_OBSERVABLE_BROADCAST_KEY);
				check_string(ret==0,nyoci_status_to_cstr(ret));
			}

			ret = nyoci_outbound_add_option_uint(
				COAP_OPTION_CONTENT_TYPE,
				COAP_CONTENT_TYPE_APPLICATION_LINK_FORMAT
			);
			require_noerr(ret, bail);

			if (max_age != UINT32_MAX) {
				ret = nyoci_outbound_add_option_uint(COAP_OPTION_MAX_AGE, max_age);
				require_noerr(ret, bail);
			}

			content_ptr = nyoci_outbound_get_content_ptr(&content_len);
			content_end_ptr = content_ptr+content_len;

			for (key_index=0; key_index < BAD_KEY_INDEX; key_index++) {
				ret = node->func(node,NYOCI_VAR_GET_KEY,key_index,buffer);
				if (ret) {
					break;
				}

				if (content_ptr + 2 >= content_end_ptr) {
					// No more room for content.
					// TODO: Figure out how to handle this case.
					break;
				}

				*content_ptr++ = '<';
				if (needs_prefix) {
					content_ptr += url_encode_cstr(content_ptr, prefix_name, (content_end_ptr-content_ptr)-1);
					content_ptr = stpncpy(content_ptr,"/",MIN(1,(content_end_ptr-content_ptr)-1));
				}
				content_ptr += url_encode_cstr(content_ptr, buffer, (content_end_ptr-content_ptr)-1);
				*content_ptr++ = '>';

				ret = node->func(node,NYOCI_VAR_GET_VALUE,key_index,buffer);

				if (content_ptr + 4 >= content_end_ptr) {
					// No more room for content.
					break;
				}

				if(!ret) {
					strcpy(content_ptr,";v=");
					content_ptr += 3;
					content_ptr += quoted_cstr(content_ptr, buffer, (content_end_ptr-content_ptr)-1);
				}

				ret = node->func(node,NYOCI_VAR_GET_LF_TITLE,key_index,buffer);

				if (content_ptr + 8 >= content_end_ptr) {
					// No more room for content.
					break;
				}

				if (!ret) {
					strcpy(content_ptr,";title=");
					content_ptr += 7;
					content_ptr += quoted_cstr(content_ptr, buffer, (content_end_ptr-content_ptr)-1);
				}

				// Observation flag
				if (0 == node->func(node,NYOCI_VAR_GET_OBSERVABLE,key_index,NULL)) {
					content_ptr = stpncpy(content_ptr,";obs",MIN(4,(content_end_ptr-content_ptr)-1));
				}

				*content_ptr++ = ',';
			}
			ret = nyoci_outbound_set_content_len(content_len-(coap_size_t)(content_end_ptr-content_ptr));
			require_noerr(ret,bail);

			ret = nyoci_outbound_send();
		} else {
			coap_size_t replyContentLength = 0;
			char *replyContent;

			ret = nyoci_outbound_begin_response(COAP_RESULT_205_CONTENT);
			require_noerr(ret,bail);

			if (0 == node->func(node,NYOCI_VAR_GET_OBSERVABLE,key_index,buffer)) {
				ret = nyoci_observable_update(&node->observable, key_index);
				check_string(ret==0,nyoci_status_to_cstr(ret));
			}

			if (reply_content_type == NYOCI_CONTENT_TYPE_APPLICATION_FORM_URLENCODED) {
				uint32_t etag;

				if (0==node->func(node,NYOCI_VAR_GET_MAX_AGE,key_index,buffer)) {
#if HAVE_STRTOL
					uint32_t max_age = strtol(buffer,NULL,0)&0xFFFFFF;
#else
					uint32_t max_age = atoi(buffer)&0xFFFFFF;
#endif
					nyoci_outbound_add_option_uint(COAP_OPTION_MAX_AGE, max_age);
				}

				ret = node->func(node,NYOCI_VAR_GET_VALUE,key_index,buffer);
				require_noerr(ret,bail);

				struct fasthash_state_s fasthash;
				fasthash_start(&fasthash, 0);
				fasthash_feed(&fasthash, (const uint8_t*)buffer, (uint8_t)strlen(buffer));
				etag = fasthash_finish_uint32(&fasthash);

				nyoci_outbound_add_option_uint(COAP_OPTION_CONTENT_TYPE, NYOCI_CONTENT_TYPE_APPLICATION_FORM_URLENCODED);

				nyoci_outbound_add_option_uint(COAP_OPTION_ETAG, etag);

				replyContent = nyoci_outbound_get_content_ptr(&replyContentLength);

				*replyContent++ = 'v';
				*replyContent++ = '=';
				replyContentLength -= 2;
				replyContentLength = (coap_size_t)url_encode_cstr(
					replyContent,
					buffer,
					replyContentLength
				);
				ret = nyoci_outbound_set_content_len(replyContentLength+2);
			} else {
				ret = node->func(node,NYOCI_VAR_GET_VALUE,key_index,buffer);
				require_noerr(ret,bail);

				ret = nyoci_outbound_append_content(buffer, NYOCI_CSTR_LEN);
			}

			require_noerr(ret,bail);

			ret = nyoci_outbound_send();
		}
	} else {
		ret = NYOCI_STATUS_NOT_ALLOWED;
	}

bail:
	check_string(ret == NYOCI_STATUS_OK, nyoci_status_to_cstr(ret));
	return ret;
}
