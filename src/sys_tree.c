/*
Copyright (c) 2009-2018 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.
*/

#ifdef WITH_SYS_TREE

#include "config.h"

#include <math.h>
#include <stdio.h>

#include "mosquitto_broker_internal.h"
#include "memory_mosq.h"
#include "time_mosq.h"

#define BUFLEN 100

#define SYS_TREE_QOS 2

uint64_t g_bytes_received = 0;
uint64_t g_bytes_sent = 0;
uint64_t g_pub_bytes_received = 0;
uint64_t g_pub_bytes_sent = 0;
unsigned long g_msgs_received = 0;
unsigned long g_msgs_sent = 0;
unsigned long g_pub_msgs_received = 0;
unsigned long g_pub_msgs_sent = 0;
unsigned long g_msgs_dropped = 0;
int g_clients_expired = 0;
unsigned int g_socket_connections = 0;
unsigned int g_connection_count = 0;

void sys_tree__init(struct mosquitto_db *db)
{
	char buf[64];

	if(db->config->sys_interval == 0){
		return;
	}

	/* Set static $SYS messages */
	snprintf(buf, 64, "mosquitto version %s", VERSION);
	db__messages_easy_queue(db, NULL, "$SYS/broker/version", SYS_TREE_QOS, strlen(buf), buf, 1);
}

static void sys_tree__update_clients(struct mosquitto_db *db, char *buf)
{
	static unsigned int client_count = -1;
	static int clients_expired = -1;
	static unsigned int client_max = 0;
	static int disconnected_count = -1;
	static int connected_count = -1;

	unsigned int count_total, count_by_sock;

	count_total = HASH_CNT(hh_id, db->contexts_by_id);
	count_by_sock = HASH_CNT(hh_sock, db->contexts_by_sock);

	if(client_count != count_total){
		client_count = count_total;
		snprintf(buf, BUFLEN, "%d", client_count);//有效和无效连接、注册到服务器上的总数
		db__messages_easy_queue(db, NULL, "$SYS/broker/clients/total", SYS_TREE_QOS, strlen(buf), buf, 1);

		if(client_count > client_max){
			client_max = client_count;
			snprintf(buf, BUFLEN, "%d", client_max);//服务器同一时间连接的最大客户端数量
			db__messages_easy_queue(db, NULL, "$SYS/broker/clients/maximum", SYS_TREE_QOS, strlen(buf), buf, 1);
		}
	}

	if(disconnected_count != count_total-count_by_sock){
		disconnected_count = count_total-count_by_sock;
		if(disconnected_count < 0){
			/* If a client has connected but not sent a CONNECT at this point,
			 * then it is possible that count_by_sock will be bigger than
			 * count_total, causing a negative number. This situation should
			 * not last for long, so just cap at zero and ignore. */
			disconnected_count = 0;
		}//注册到服务器上的持久连接（clean seesion为false)但当前断开的客户端数量
		snprintf(buf, BUFLEN, "%d", disconnected_count);
		db__messages_easy_queue(db, NULL, "$SYS/broker/clients/inactive", SYS_TREE_QOS, strlen(buf), buf, 1);
		db__messages_easy_queue(db, NULL, "$SYS/broker/clients/disconnected", SYS_TREE_QOS, strlen(buf), buf, 1);
	}
	if(connected_count != count_by_sock){
		connected_count = count_by_sock;
		snprintf(buf, BUFLEN, "%d", connected_count);//当前连接的客户端数量
		db__messages_easy_queue(db, NULL, "$SYS/broker/clients/active", SYS_TREE_QOS, strlen(buf), buf, 1);
		db__messages_easy_queue(db, NULL, "$SYS/broker/clients/connected", SYS_TREE_QOS, strlen(buf), buf, 1);
	}
	if(g_clients_expired != clients_expired){
		clients_expired = g_clients_expired;//超过有效期被断开连接的客户端数量
		snprintf(buf, BUFLEN, "%d", clients_expired);//有效期通过persistent_client_expiration设置
		db__messages_easy_queue(db, NULL, "$SYS/broker/clients/expired", SYS_TREE_QOS, strlen(buf), buf, 1);
	}
}

#ifdef REAL_WITH_MEMORY_TRACKING
static void sys_tree__update_memory(struct mosquitto_db *db, char *buf)
{
	static unsigned long current_heap = -1;
	static unsigned long max_heap = -1;
	unsigned long value_ul;

	value_ul = mosquitto__memory_used();
	if(current_heap != value_ul){
		current_heap = value_ul;
		snprintf(buf, BUFLEN, "%lu", current_heap);//正在使用的堆内存大小
		db__messages_easy_queue(db, NULL, "$SYS/broker/heap/current", SYS_TREE_QOS, strlen(buf), buf, 1);
	}
	value_ul = mosquitto__max_memory_used();
	if(max_heap != value_ul){
		max_heap = value_ul;
		snprintf(buf, BUFLEN, "%lu", max_heap);//使用的最大堆内存
		db__messages_easy_queue(db, NULL, "$SYS/broker/heap/maximum", SYS_TREE_QOS, strlen(buf), buf, 1);
	}
}
#endif

static void calc_load(struct mosquitto_db *db, char *buf, const char *topic, bool initial, double exponent, double interval, double *current)
{
	double new_value;

	if (initial) {
		new_value = *current;
		snprintf(buf, BUFLEN, "%.2f", new_value);
		db__messages_easy_queue(db, NULL, topic, SYS_TREE_QOS, strlen(buf), buf, 1);
	} else {
		new_value = interval + exponent*((*current) - interval);
		if(fabs(new_value - (*current)) >= 0.01){
			snprintf(buf, BUFLEN, "%.2f", new_value);
			db__messages_easy_queue(db, NULL, topic, SYS_TREE_QOS, strlen(buf), buf, 1);
		}
	}
	(*current) = new_value;
}

/* Send messages for the $SYS hierarchy if the last update is longer than
 * 'interval' seconds ago.
 * 'interval' is the amount of seconds between updates. If 0, then no periodic
 * messages are sent for the $SYS hierarchy.
 * 'start_time' is the result of time() that the broker was started at.
 */
void sys_tree__update(struct mosquitto_db *db, int interval, time_t start_time)
{
	static time_t last_update = 0;
	time_t now;
	time_t uptime;
	char buf[BUFLEN];

	static int msg_store_count = -1;
	static unsigned long msg_store_bytes = -1;
	static unsigned long msgs_received = -1;
	static unsigned long msgs_sent = -1;
	static unsigned long publish_dropped = -1;
	static unsigned long pub_msgs_received = -1;
	static unsigned long pub_msgs_sent = -1;
	static unsigned long long bytes_received = -1;
	static unsigned long long bytes_sent = -1;
	static unsigned long long pub_bytes_received = -1;
	static unsigned long long pub_bytes_sent = -1;
	static int subscription_count = -1;
	static int retained_count = -1;

	static double msgs_received_load1 = 0;
	static double msgs_received_load5 = 0;
	static double msgs_received_load15 = 0;
	static double msgs_sent_load1 = 0;
	static double msgs_sent_load5 = 0;
	static double msgs_sent_load15 = 0;
	static double publish_dropped_load1 = 0;
	static double publish_dropped_load5 = 0;
	static double publish_dropped_load15 = 0;
	double msgs_received_interval, msgs_sent_interval, publish_dropped_interval;

	static double publish_received_load1 = 0;
	static double publish_received_load5 = 0;
	static double publish_received_load15 = 0;
	static double publish_sent_load1 = 0;
	static double publish_sent_load5 = 0;
	static double publish_sent_load15 = 0;
	double publish_received_interval, publish_sent_interval;

	static double bytes_received_load1 = 0;
	static double bytes_received_load5 = 0;
	static double bytes_received_load15 = 0;
	static double bytes_sent_load1 = 0;
	static double bytes_sent_load5 = 0;
	static double bytes_sent_load15 = 0;
	double bytes_received_interval, bytes_sent_interval;

	static double socket_load1 = 0;
	static double socket_load5 = 0;
	static double socket_load15 = 0;
	double socket_interval;

	static double connection_load1 = 0;
	static double connection_load5 = 0;
	static double connection_load15 = 0;
	double connection_interval;

	double exponent;
	double i_mult;

	now = mosquitto_time();

	if(interval && now - interval > last_update){
		uptime = now - start_time;
		snprintf(buf, BUFLEN, "%d seconds", (int)uptime);//服务器启动时长
		db__messages_easy_queue(db, NULL, "$SYS/broker/uptime", SYS_TREE_QOS, strlen(buf), buf, 1);

		sys_tree__update_clients(db, buf);
		bool initial_publish = false;
		if(last_update == 0){
			initial_publish = true;
			last_update = 1;
		}
		if(last_update > 0){
			i_mult = 60.0/(double)(now-last_update);

			msgs_received_interval = (g_msgs_received - msgs_received)*i_mult;
			msgs_sent_interval = (g_msgs_sent - msgs_sent)*i_mult;
			publish_dropped_interval = (g_msgs_dropped - publish_dropped)*i_mult;

			publish_received_interval = (g_pub_msgs_received - pub_msgs_received)*i_mult;
			publish_sent_interval = (g_pub_msgs_sent - pub_msgs_sent)*i_mult;

			bytes_received_interval = (g_bytes_received - bytes_received)*i_mult;
			bytes_sent_interval = (g_bytes_sent - bytes_sent)*i_mult;

			socket_interval = g_socket_connections*i_mult;
			g_socket_connections = 0;
			connection_interval = g_connection_count*i_mult;
			g_connection_count = 0;

			/* 1 minute load */
			exponent = exp(-1.0*(now-last_update)/60.0);
			//1分钟内服务器接收到的所有类型消息的平均数
			calc_load(db, buf, "$SYS/broker/load/messages/received/1min", initial_publish, exponent, msgs_received_interval, &msgs_received_load1);
			calc_load(db, buf, "$SYS/broker/load/messages/sent/1min", initial_publish, exponent, msgs_sent_interval, &msgs_sent_load1);
			//1分钟内服务器丢弃的消息的平均数，这表明了那些持久连接但与服务器断开的客户端失去消息的速率
			calc_load(db, buf, "$SYS/broker/load/publish/dropped/1min", initial_publish, exponent, publish_dropped_interval, &publish_dropped_load1);
			//1分钟内服务器接收的发布消息的平均数
			calc_load(db, buf, "$SYS/broker/load/publish/received/1min", initial_publish, exponent, publish_received_interval, &publish_received_load1);
			//1分钟内服务器发送的发布消息的平均数
			calc_load(db, buf, "$SYS/broker/load/publish/sent/1min", initial_publish, exponent, publish_sent_interval, &publish_sent_load1);
			//1分钟内服务器接收数据的平均字节数，以下类似
			calc_load(db, buf, "$SYS/broker/load/bytes/received/1min", initial_publish, exponent, bytes_received_interval, &bytes_received_load1);
			calc_load(db, buf, "$SYS/broker/load/bytes/sent/1min", initial_publish, exponent, bytes_sent_interval, &bytes_sent_load1);
			//1分钟内服务器打开的socket连接的平均数
			calc_load(db, buf, "$SYS/broker/load/sockets/1min", initial_publish, exponent, socket_interval, &socket_load1);
			//1分钟内服务器接收到的connections包的平均数
			calc_load(db, buf, "$SYS/broker/load/connections/1min", initial_publish, exponent, connection_interval, &connection_load1);

			/* 5 minute load */
			exponent = exp(-1.0*(now-last_update)/300.0);

			calc_load(db, buf, "$SYS/broker/load/messages/received/5min", initial_publish, exponent, msgs_received_interval, &msgs_received_load5);
			calc_load(db, buf, "$SYS/broker/load/messages/sent/5min", initial_publish, exponent, msgs_sent_interval, &msgs_sent_load5);
			calc_load(db, buf, "$SYS/broker/load/publish/dropped/5min", initial_publish, exponent, publish_dropped_interval, &publish_dropped_load5);
			calc_load(db, buf, "$SYS/broker/load/publish/received/5min", initial_publish, exponent, publish_received_interval, &publish_received_load5);
			calc_load(db, buf, "$SYS/broker/load/publish/sent/5min", initial_publish, exponent, publish_sent_interval, &publish_sent_load5);
			calc_load(db, buf, "$SYS/broker/load/bytes/received/5min", initial_publish, exponent, bytes_received_interval, &bytes_received_load5);
			calc_load(db, buf, "$SYS/broker/load/bytes/sent/5min", initial_publish, exponent, bytes_sent_interval, &bytes_sent_load5);
			calc_load(db, buf, "$SYS/broker/load/sockets/5min", initial_publish, exponent, socket_interval, &socket_load5);
			calc_load(db, buf, "$SYS/broker/load/connections/5min", initial_publish, exponent, connection_interval, &connection_load5);

			/* 15 minute load */
			exponent = exp(-1.0*(now-last_update)/900.0);

			calc_load(db, buf, "$SYS/broker/load/messages/received/15min", initial_publish, exponent, msgs_received_interval, &msgs_received_load15);
			calc_load(db, buf, "$SYS/broker/load/messages/sent/15min", initial_publish, exponent, msgs_sent_interval, &msgs_sent_load15);
			calc_load(db, buf, "$SYS/broker/load/publish/dropped/15min", initial_publish, exponent, publish_dropped_interval, &publish_dropped_load15);
			calc_load(db, buf, "$SYS/broker/load/publish/received/15min", initial_publish, exponent, publish_received_interval, &publish_received_load15);
			calc_load(db, buf, "$SYS/broker/load/publish/sent/15min", initial_publish, exponent, publish_sent_interval, &publish_sent_load15);
			calc_load(db, buf, "$SYS/broker/load/bytes/received/15min", initial_publish, exponent, bytes_received_interval, &bytes_received_load15);
			calc_load(db, buf, "$SYS/broker/load/bytes/sent/15min", initial_publish, exponent, bytes_sent_interval, &bytes_sent_load15);
			calc_load(db, buf, "$SYS/broker/load/sockets/15min", initial_publish, exponent, socket_interval, &socket_load15);
			calc_load(db, buf, "$SYS/broker/load/connections/15min", initial_publish, exponent, connection_interval, &connection_load15);
		}

		if(db->msg_store_count != msg_store_count){
			msg_store_count = db->msg_store_count;//服务器存储的消息的总数
			snprintf(buf, BUFLEN, "%d", msg_store_count);//包括保留消息和持久连接客户端的消息队列中的消息数
			db__messages_easy_queue(db, NULL, "$SYS/broker/messages/stored", SYS_TREE_QOS, strlen(buf), buf, 1);
			db__messages_easy_queue(db, NULL, "$SYS/broker/store/messages/count", SYS_TREE_QOS, strlen(buf), buf, 1);
		}

		if (db->msg_store_bytes != msg_store_bytes){
			msg_store_bytes = db->msg_store_bytes;
			snprintf(buf, BUFLEN, "%lu", msg_store_bytes);
			db__messages_easy_queue(db, NULL, "$SYS/broker/store/messages/bytes", SYS_TREE_QOS, strlen(buf), buf, 1);
		}

		if(db->subscription_count != subscription_count){
			subscription_count = db->subscription_count;
			snprintf(buf, BUFLEN, "%d", subscription_count);//服务器订阅主题总数
			db__messages_easy_queue(db, NULL, "$SYS/broker/subscriptions/count", SYS_TREE_QOS, strlen(buf), buf, 1);
		}

		if(db->retained_count != retained_count){
			retained_count = db->retained_count;
			snprintf(buf, BUFLEN, "%d", retained_count);//服务器保留的消息总数
			db__messages_easy_queue(db, NULL, "$SYS/broker/retained messages/count", SYS_TREE_QOS, strlen(buf), buf, 1);
		}

#ifdef REAL_WITH_MEMORY_TRACKING
		sys_tree__update_memory(db, buf);
#endif

		if(msgs_received != g_msgs_received){
			msgs_received = g_msgs_received;
			snprintf(buf, BUFLEN, "%lu", msgs_received);//自服务器启动以来接收的所有类型的消息总数
			db__messages_easy_queue(db, NULL, "$SYS/broker/messages/received", SYS_TREE_QOS, strlen(buf), buf, 1);
		}
		
		if(msgs_sent != g_msgs_sent){
			msgs_sent = g_msgs_sent;
			snprintf(buf, BUFLEN, "%lu", msgs_sent);//自服务器启动以来发送的所有类型的消息总数
			db__messages_easy_queue(db, NULL, "$SYS/broker/messages/sent", SYS_TREE_QOS, strlen(buf), buf, 1);
		}

		if(publish_dropped != g_msgs_dropped){
			publish_dropped = g_msgs_dropped;//由于inflight/queued限制而直接丢弃的消息的总数
			snprintf(buf, BUFLEN, "%lu", publish_dropped);
			db__messages_easy_queue(db, NULL, "$SYS/broker/publish/messages/dropped", SYS_TREE_QOS, strlen(buf), buf, 1);
		}

		if(pub_msgs_received != g_pub_msgs_received){
			pub_msgs_received = g_pub_msgs_received;//自服务器启动以来接收的发布消息的总数
			snprintf(buf, BUFLEN, "%lu", pub_msgs_received);
			db__messages_easy_queue(db, NULL, "$SYS/broker/publish/messages/received", SYS_TREE_QOS, strlen(buf), buf, 1);
		}
		
		if(pub_msgs_sent != g_pub_msgs_sent){
			pub_msgs_sent = g_pub_msgs_sent;//自服务器启动以来发送的发布消息的总数
			snprintf(buf, BUFLEN, "%lu", pub_msgs_sent);
			db__messages_easy_queue(db, NULL, "$SYS/broker/publish/messages/sent", SYS_TREE_QOS, strlen(buf), buf, 1);
		}

		if(bytes_received != g_bytes_received){
			bytes_received = g_bytes_received;
			snprintf(buf, BUFLEN, "%llu", bytes_received);
			db__messages_easy_queue(db, NULL, "$SYS/broker/bytes/received", SYS_TREE_QOS, strlen(buf), buf, 1);
		}
		
		if(bytes_sent != g_bytes_sent){
			bytes_sent = g_bytes_sent;
			snprintf(buf, BUFLEN, "%llu", bytes_sent);
			db__messages_easy_queue(db, NULL, "$SYS/broker/bytes/sent", SYS_TREE_QOS, strlen(buf), buf, 1);
		}
		
		if(pub_bytes_received != g_pub_bytes_received){
			pub_bytes_received = g_pub_bytes_received;
			snprintf(buf, BUFLEN, "%llu", pub_bytes_received);
			db__messages_easy_queue(db, NULL, "$SYS/broker/publish/bytes/received", SYS_TREE_QOS, strlen(buf), buf, 1);
		}

		if(pub_bytes_sent != g_pub_bytes_sent){
			pub_bytes_sent = g_pub_bytes_sent;
			snprintf(buf, BUFLEN, "%llu", pub_bytes_sent);
			db__messages_easy_queue(db, NULL, "$SYS/broker/publish/bytes/sent", SYS_TREE_QOS, strlen(buf), buf, 1);
		}

		last_update = mosquitto_time();
	}
}

#endif
