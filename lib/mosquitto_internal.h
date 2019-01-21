/*
Copyright (c) 2010-2018 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.
   Tatsuzo Osawa - Add epoll.
*/

#ifndef MOSQUITTO_INTERNAL_H
#define MOSQUITTO_INTERNAL_H

#include "config.h"

#ifdef WIN32
#  include <winsock2.h>
#endif

#ifdef WITH_TLS
#  include <openssl/ssl.h>
#else
#  include <time.h>
#endif
#include <stdlib.h>

#if defined(WITH_THREADING) && !defined(WITH_BROKER)
#  include <pthread.h>
#else
#  include <dummypthread.h>
#endif

#ifdef WITH_SRV
#  include <ares.h>
#endif

#ifdef WIN32
#	if _MSC_VER < 1600
		typedef unsigned char uint8_t;
		typedef unsigned short uint16_t;
		typedef unsigned int uint32_t;
		typedef unsigned long long uint64_t;
#	else
#		include <stdint.h>
#	endif
#else
#	include <stdint.h>
#endif

#include "mosquitto.h"
#include "time_mosq.h"
#ifdef WITH_BROKER
#  ifdef __linux__
#    include <netdb.h>
#  endif
#  include "uthash.h"
struct mosquitto_client_msg;
#endif

#ifdef WIN32
typedef SOCKET mosq_sock_t;
#else
typedef int mosq_sock_t;
#endif

enum mosquitto_msg_direction {
	mosq_md_in = 0,
	mosq_md_out = 1
};

enum mosquitto_msg_state {
	mosq_ms_invalid = 0,
	mosq_ms_publish_qos0 = 1,
	mosq_ms_publish_qos1 = 2,
	mosq_ms_wait_for_puback = 3,//Oos==1时，发送PUBLISH后等待PUBACK返回
	mosq_ms_publish_qos2 = 4,
	mosq_ms_wait_for_pubrec = 5,//Oos==2时，发送PUBLISH后，等待PUBREC返回
	mosq_ms_resend_pubrel = 6,
	mosq_ms_wait_for_pubrel = 7,//Oos==2时，发送PUBREC后等待PUBREL返回
	mosq_ms_resend_pubcomp = 8,
	mosq_ms_wait_for_pubcomp = 9, //Oos==2时，发送PUBREL后等待PUBCOMP返回
	mosq_ms_send_pubrec = 10,
	mosq_ms_queued = 11
};
//该状态为用户端连接成功并通讯CONNECT之后结果
enum mosquitto_client_state {
	mosq_cs_new = 0,
	mosq_cs_connected = 1,
	mosq_cs_disconnecting = 2,
	mosq_cs_connect_async = 3,
	mosq_cs_connect_pending = 4,
	mosq_cs_connect_srv = 5,
	mosq_cs_disconnect_ws = 6,
	mosq_cs_disconnected = 7,
	mosq_cs_socks5_new = 8,
	mosq_cs_socks5_start = 9,
	mosq_cs_socks5_request = 10,
	mosq_cs_socks5_reply = 11,
	mosq_cs_socks5_auth_ok = 12,
	mosq_cs_socks5_userpass_reply = 13,
	mosq_cs_socks5_send_userpass = 14,
	mosq_cs_expiring = 15,
	mosq_cs_connecting = 16,
	mosq_cs_duplicate = 17, /* client that has been taken over by another with the same id */
};

enum mosquitto__protocol {
	mosq_p_invalid = 0,
	mosq_p_mqtt31 = 1,
	mosq_p_mqtt311 = 2,
	mosq_p_mqtts = 3
};

enum mosquitto__threaded_state {
	mosq_ts_none,		/* No threads in use */
	mosq_ts_self,		/* Threads started by libmosquitto */
	mosq_ts_external	/* Threads started by external code */
};

enum mosquitto__transport {
	mosq_t_invalid = 0,
	mosq_t_tcp = 1,
	mosq_t_ws = 2,
	mosq_t_sctp = 3
};
//发送数据（组包后）或者接受数据后（解包前）状态
struct mosquitto__packet{
	uint8_t *payload;
	struct mosquitto__packet *next;
	uint32_t remaining_mult;
	uint32_t remaining_length;
	uint32_t packet_length;
	uint32_t to_process;//发送进度，记录还未发送多少字节，缺省为packet_length
	uint32_t pos;//组包或者发送时用到，发送时记录发送到什么位置
	uint16_t mid;//消息id，当Qos==0 时回调on_publish时用
	uint8_t command;//包头
	/* remaining_count对应固定头的剩余长度的字节数.有三种状态:
	 *   0 没有读到任何剩余长度字节
	 *   <0 读到部分剩余长度字节，但还没读完
	 *   >0 剩余长度字节全部读完.*/
	int8_t remaining_count;
};

struct mosquitto_message_all{
	struct mosquitto_message_all *next;
	time_t timestamp;//时间，记录本地软件tick时间
	//enum mosquitto_msg_direction direction;
	enum mosquitto_msg_state state;
	bool dup;
	struct mosquitto_message msg;
};
//用于保存一个客户端连接的所有信息，例如用户名、密码、用户ID、向该客户端发送的消息等
struct mosquitto {
	mosq_sock_t sock;//mosquitto服务器程序与该客户端连接通信所用的socket描述符
#ifndef WITH_BROKER
	mosq_sock_t sockpairR, sockpairW;//客户端用，socket管道通知：非阻塞模式时，通知用，在mosquitto_loop 调用发送
#endif
#if defined(__GLIBC__) && defined(WITH_ADNS)
	struct gaicb *adns; /* For getaddrinfo_a */
#endif
	enum mosquitto__protocol protocol;/*客户端使用的协议版本号*/
	char *address;//该客户端的IP地址
	char *id;//该客户端登陆mosquitto程序时所提供的ID值，该值与其他的客户端不能重复
	char *username;//客户端登陆时所提供的用户名
	char *password;//密码,鉴权使用
	uint16_t keepalive;//该客户端需在此时间内向mosquitto服务器程序发送一条ping/pong消息
	uint16_t last_mid;//最后一个消息id，发消息后++
	enum mosquitto_client_state state;//客户端的状态
	time_t last_msg_in;//上次收发消息的时间
	time_t next_msg_out;
	time_t ping_t;
	struct mosquitto__packet in_packet;//收到的包
	struct mosquitto__packet *current_out_packet;//当前处理（发送）的节点
	/*发送数据包队列，所有msgs中方向为mosq_md_out的消息，
	最终会格式化成报文添加到out_packet中，然后发送出去，
	该指针指向第一个要发送的消息*/	
	struct mosquitto__packet *out_packet;
	struct mosquitto_message *will;//遗嘱
#ifdef WITH_TLS
	SSL *ssl;
	SSL_CTX *ssl_ctx;
	char *tls_cafile;
	char *tls_capath;
	char *tls_certfile;
	char *tls_keyfile;
	int (*tls_pw_callback)(char *buf, int size, int rwflag, void *userdata);
	char *tls_version;
	char *tls_ciphers;
	char *tls_psk;
	char *tls_psk_identity;
	int tls_cert_reqs;
	bool tls_insecure;
	bool ssl_ctx_defaults;
#endif
	bool want_write;
	bool want_connect;
#if defined(WITH_THREADING) && !defined(WITH_BROKER)
	pthread_mutex_t callback_mutex;
	pthread_mutex_t log_callback_mutex;
	pthread_mutex_t msgtime_mutex;
	pthread_mutex_t out_packet_mutex;
	pthread_mutex_t current_out_packet_mutex;
	pthread_mutex_t state_mutex;
	pthread_mutex_t in_message_mutex;
	pthread_mutex_t out_message_mutex;
	pthread_mutex_t mid_mutex;
	pthread_t thread_id;
#endif
	bool clean_session;/*是否清空之前的会话信息*/
#ifdef WITH_BROKER
	char *old_id; /* for when a duplicate client connects, but we still want to
					 know what the id was */
	bool is_dropping;
	bool is_bridge;
	struct mosquitto__bridge *bridge;
	struct mosquitto_client_msg *inflight_msgs;//交互消息队列头指针
	struct mosquitto_client_msg *last_inflight_msg;//交互消息队列尾部指针
	struct mosquitto_client_msg *queued_msgs;//等待消息队列头指针
	struct mosquitto_client_msg *last_queued_msg;//等待消息队列尾部指针
	unsigned long msg_bytes;
	unsigned long msg_bytes12;
	int msg_count;
	int msg_count12;
	struct mosquitto__acl_user *acl_list;
	struct mosquitto__listener *listener;
	time_t disconnect_t;
	struct mosquitto__packet *out_packet_last;//标记待发送packet队列的末尾一个元素
	struct mosquitto__subhier **subs;/*该客户端订阅的主题链表*/
	int sub_count;
	int pollfd_index;
#  ifdef WITH_WEBSOCKETS
#    if defined(LWS_LIBRARY_VERSION_NUMBER)
	struct lws *wsi;
#    else
	struct libwebsocket_context *ws_context;
	struct libwebsocket *wsi;
#    endif
#  endif
	bool ws_want_write;
#else
#  ifdef WITH_SOCKS
	char *socks5_host;
	int socks5_port;
	char *socks5_username;
	char *socks5_password;
#  endif
	void *userdata;
	bool in_callback;
	struct mosquitto_message_all *in_messages;
	struct mosquitto_message_all *in_messages_last;
	struct mosquitto_message_all *out_messages;
	struct mosquitto_message_all *out_messages_last;
	void (*on_connect)(struct mosquitto *, void *userdata, int rc);
	void (*on_connect_with_flags)(struct mosquitto *, void *userdata, int rc, int flags);
	void (*on_disconnect)(struct mosquitto *, void *userdata, int rc);
	void (*on_publish)(struct mosquitto *, void *userdata, int mid);
	void (*on_message)(struct mosquitto *, void *userdata, const struct mosquitto_message *message);
	void (*on_subscribe)(struct mosquitto *, void *userdata, int mid, int qos_count, const int *granted_qos);
	void (*on_unsubscribe)(struct mosquitto *, void *userdata, int mid);
	void (*on_log)(struct mosquitto *, void *userdata, int level, const char *str);
	//void (*on_error)();
	char *host;
	int port;
	int in_queue_len;
	int out_queue_len;
	char *bind_address;
	unsigned int reconnect_delay;
	unsigned int reconnect_delay_max;
	bool reconnect_exponential_backoff;
	char threaded;
	struct mosquitto__packet *out_packet_last;
	int inflight_messages;
	int max_inflight_messages;
#  ifdef WITH_SRV
	ares_channel achan;// ares_init函数中初始化，返回一个交流通道
#  endif
#endif

#ifdef WITH_BROKER
	UT_hash_handle hh_id;
	UT_hash_handle hh_sock;
	struct mosquitto *for_free_next;
#endif
#ifdef WITH_EPOLL
	uint32_t events;
#endif
};

#define STREMPTY(str) (str[0] == '\0')

#endif

