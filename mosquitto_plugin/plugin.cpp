#include "mosquitto_broker.h"
#include "mosquitto_plugin.h"
#include "mosquitto.h"
#include "mqtt_protocol.h"

#include <mqueue.h> 
#include <errno.h>

#include <iostream>
#include <fstream>
#include <string>
#include "ydb-global.h"
#include <regex>
#include "json.h"
#include <vector>
#include <sstream>
#include <chrono>

using namespace::std;
using namespace::std::chrono;

steady_clock::time_point timepoint_first_synchronisation;
int synchronisation_counter_maximum = 1000;
int synchronisation_counter = 0;

string sync_mode = "client";

bool time_measurement_trigger_to_publish = false;

ofstream time_log_mq_trigger_to_publish;
ofstream time_log_global_trigger_to_publish;
ofstream time_log_client_trigger_to_publish;

c_ydb_global _globalSyncBuffer("^globalSyncBuffer"); 
c_ydb_global dummy("dummy");

mqd_t mq_descriptor = -1;

struct mq_attr mq_attributes = {
    .mq_flags = 0,
    .mq_maxmsg = 10,
    .mq_msgsize = 8192,
    .mq_curmsgs = 0
};

int max_mq_receive_per_tick = 300;

Json::CharReaderBuilder char_reader_builder;
Json::StreamWriterBuilder stream_writer_builder;
Json::CharReader *char_reader;
c_ydb_global _articles("^articles");


static int callback_message(int event, void *event_data, void *userdata);

static int callback_tick(int event, void *event_data, void *userdata);

int get_global_sync_buffer_data();

int receive_and_publish_mq_messages();

bool publish_mqtt_message(string topic, Json::Value &payload); 

bool publish_mqtt_message(string topic, string payload);

int64_t get_time_difference_in_nano(int64_t start_duration_rep);

void print_time_difference_first_to_last_synchronisation();

const int QOS = 2;
const bool RETAIN = false;
mosquitto_property *PROPERTIES = NULL;

static mosquitto_plugin_id_t * mosq_pid = NULL;

int mosquitto_plugin_version(int supported_version_count, const int *supported_versions)
{
	mosquitto_log_printf(MOSQ_LOG_INFO, "mosquitto_plugin_version\n");
	
	for(int i=0; i<supported_version_count; i++) {
		if(supported_versions[i] == 5)
			return 5;
	}
	
	return -1;
}

int mosquitto_plugin_init(mosquitto_plugin_id_t *identifier, void **user_data, struct mosquitto_opt *opts, int opt_count)
{
	mosq_pid = identifier;

	mosquitto_log_printf(MOSQ_LOG_INFO, "Init %d\n", opt_count);

	char_reader = char_reader_builder.newCharReader();

	for (int i = 0; i < opt_count; i++) {
		if(!strcmp(opts[i].key, "sync_mode")) {
			if(!strcmp(opts[i].value,"client") || !strcmp(opts[i].value,"mq") || !strcmp(opts[i].value,"global")) {
				mosquitto_log_printf(MOSQ_LOG_INFO, "Selected Sync Mode: %s", opts[i].value);
				sync_mode = string(opts[i].value);
			}
			else {
				mosquitto_log_printf(MOSQ_LOG_INFO, "Invalid Sync Mode %s" , sync_mode);
			}
		}
		else if(!strcmp(opts[i].key, "time_measurement_trigger_to_publish")) {
			if(!strcmp(opts[i].value, "true")) {
				time_measurement_trigger_to_publish = true;
			}
		}
	}

	if(sync_mode == "mq") {
		mq_descriptor = mq_open("/mqsync", O_RDONLY | O_CREAT | O_NONBLOCK, S_IRWXU, &mq_attributes); 

		if(mq_descriptor == -1)
			return MOSQ_ERR_UNKNOWN;
	}

	if(time_measurement_trigger_to_publish){
		if(sync_mode == "mq") {
			time_log_mq_trigger_to_publish.open("/home/emi/ydbay/time_measurements/mq/trigger_to_publish"); 
		}
		if(sync_mode == "global"){
			time_log_global_trigger_to_publish.open("/home/emi/ydbay/time_measurements/global/trigger_to_publish");
		}
		if(sync_mode == "client"){
			time_log_client_trigger_to_publish.open("/home/emi/ydbay/time_measurements/client/trigger_to_publish");
		}
	}

	return mosquitto_callback_register(mosq_pid, MOSQ_EVT_TICK, callback_tick, NULL, NULL)
		|| mosquitto_callback_register(mosq_pid, MOSQ_EVT_MESSAGE, callback_message, NULL, NULL);
}

int mosquitto_plugin_cleanup(void *user_data, struct mosquitto_opt *opts, int opt_count)
{
	if(sync_mode == "mq") {
		if(mq_descriptor != -1) {
			mq_close(mq_descriptor);
		}
	}

	if(time_measurement_trigger_to_publish){
		if(sync_mode == "mq"){
			time_log_mq_trigger_to_publish.close();
		}
		if(sync_mode == "global"){
			time_log_global_trigger_to_publish.close();
		}
		if(sync_mode == "client"){
			time_log_client_trigger_to_publish.close();
		}
	}

	return mosquitto_callback_unregister(mosq_pid, MOSQ_EVT_BASIC_AUTH, callback_message, NULL)
	|| mosquitto_callback_unregister(mosq_pid, MOSQ_EVT_TICK, callback_tick, NULL);
}

// Callback - Funktionen
static int callback_message(int event, void *event_data, void *userdata) 
{
	struct mosquitto_evt_message *ed = (mosquitto_evt_message*)event_data; 

	if(!regex_match(ed->topic, regex("(mqttfetch/aabay/)([^/]+)(/fr/)([0-9]+)"))) { 
		if(sync_mode == "client") {

			if(time_measurement_trigger_to_publish) {
				if(synchronisation_counter == 0){
					timepoint_first_synchronisation = steady_clock::now();
				}

				synchronisation_counter += 1;

				char *payload = (char*)ed->payload; 
				int64_t start_duration_count = strtoll(payload, NULL, 10);

				time_log_client_trigger_to_publish << get_time_difference_in_nano(start_duration_count) << endl;

				if(synchronisation_counter == synchronisation_counter_maximum){
					print_time_difference_first_to_last_synchronisation();
					synchronisation_counter = 0;
				}
			}
		}

		return MOSQ_ERR_SUCCESS;
	}

	string response_topic = regex_replace(ed->topic, regex("/fr/"), "/to/");

	const char *client_id = mosquitto_client_id(ed->client);

	Json::Value request_payload;
	Json::Value response_payload;

	response_payload["rc"] = 0;

	char* payload = (char*)ed->payload;
	bool parse_result = char_reader->parse(payload, payload + strlen(payload), &request_payload, NULL);

	if(!parse_result)
		return MOSQ_ERR_SUCCESS;

	if(request_payload["action"] == "get_articles") {
		string iterator = "";
		int json_array_index = 0;
		
		while(iterator=_articles[iterator].nextSibling(), iterator!="") {
			response_payload["articles"][json_array_index]["id"] = iterator;
			response_payload["articles"][json_array_index]["title"] = (string)_articles[iterator]["title"];
			response_payload["articles"][json_array_index]["bid"] = (string)_articles[iterator]["bid"];
			json_array_index += 1;
		}

		publish_mqtt_message(response_topic, response_payload);

		return MOSQ_ERR_SUCCESS;
	} 

	if(request_payload["action"] == "get_article") {
		string requested_article_id = request_payload["id"].asString(); 

		if(_articles[requested_article_id].hasChilds()) {
			response_payload["article"]["id"] = requested_article_id;
			response_payload["article"]["title"] = (string)_articles[requested_article_id]["title"];
			response_payload["article"]["bid"] = (string)_articles[requested_article_id]["bid"];
			response_payload["article"]["text"] = (string)_articles[requested_article_id]["text"];
		}
		else {
			response_payload["rc"] = -1;
		}

		publish_mqtt_message(response_topic, response_payload);
	
		return MOSQ_ERR_SUCCESS;
	}

	if(request_payload["action"] == "bid") { 
		string article_id = request_payload["id"].asString();
		string nickname = request_payload["nickname"].asString(); 

		int bid = stoi(request_payload["bid"].asString()); 
		int maxbid = stoi(_articles[article_id]["maxbid"]); 

		if(!_articles[article_id].hasChilds()) {
			response_payload["rc"] = -3;
			publish_mqtt_message(response_topic, response_payload);

			return MOSQ_ERR_SUCCESS;
		}

		if(nickname == (string)_articles[article_id]["winner"]) { // Gebot stammt von Hoechstbieter
			if(bid >= maxbid + 1) {
				_articles[article_id]["maxbid"] = bid;
				_articles[article_id]["client"] = client_id; 
			}
			else {
				response_payload["rc"] = -1;
			}

			publish_mqtt_message(response_topic, response_payload);

			return MOSQ_ERR_SUCCESS;
		}

		if(bid >= maxbid + 1) { // erfolgreich ueberboten
			response_payload["rc"] = 1;

			publish_mqtt_message(response_topic, response_payload);

			string previous_winner_response_topic = "mqttfetch/aabay/" + (string)_articles[article_id]["client"] + "/to/-1";

			Json::Value previous_winner_response_payload;
			previous_winner_response_payload["rc"] = -1;

			publish_mqtt_message(previous_winner_response_topic, previous_winner_response_payload);

			string bid_notice_topic = "aabay/bids/" + article_id;
			string bid_notice_payload = to_string(maxbid+1); 
			publish_mqtt_message(bid_notice_topic, bid_notice_payload);

			_articles[article_id]["bid"] = maxbid + 1;
			_articles[article_id]["maxbid"] = bid;
			_articles[article_id]["winner"] = nickname;
			_articles[article_id]["client"] = client_id;

			return MOSQ_ERR_SUCCESS;
		}

		if(bid > stoi(_articles[article_id]["bid"])) {  // neues Gebot niedriger als hoechstgebot. Gebot wird erhoeht.
			_articles[article_id]["bid"] = bid;

			string bid_notice_topic = "aabay/bids/" + article_id;
			string bid_notice_payload = to_string(bid);
			publish_mqtt_message(bid_notice_topic, bid_notice_payload);

			response_payload["rc"] = -2;

			publish_mqtt_message(response_topic, response_payload);
			return MOSQ_ERR_SUCCESS;
		}

		return MOSQ_ERR_SUCCESS;
	} 

	response_payload["rc"] = -1;

	publish_mqtt_message(response_topic, response_payload);

	return MOSQ_ERR_SUCCESS;
}

static int callback_tick(int event, void *event_data, void *userdata) 
{
	if(sync_mode == "mq") {
			receive_and_publish_mq_messages(); 
			return MOSQ_ERR_SUCCESS;
	}

	else if(sync_mode == "global") {
			get_global_sync_buffer_data();
			return MOSQ_ERR_SUCCESS;
	}

	else { // sync_mode = "client"
		return MOSQ_ERR_SUCCESS;
	}
}

// Auslese - Funktionen
int get_global_sync_buffer_data()
{
	if(!_globalSyncBuffer.hasChilds())
		return MOSQ_ERR_SUCCESS;

	int lock_inc_result =_globalSyncBuffer.lock_inc(0);

	if(lock_inc_result != YDB_OK)  // Lock konnte nicht gesetzt werden
		return MOSQ_ERR_SUCCESS;

	string iterator_globalSyncBuffer = "";

	while(iterator_globalSyncBuffer = _globalSyncBuffer[iterator_globalSyncBuffer].nextSibling(), iterator_globalSyncBuffer != "") { 
		dummy[iterator_globalSyncBuffer] = iterator_globalSyncBuffer;		
		dummy[iterator_globalSyncBuffer]["topic"] = (string)_globalSyncBuffer[iterator_globalSyncBuffer]["topic"];
		dummy[iterator_globalSyncBuffer]["payload"] = (string)_globalSyncBuffer[iterator_globalSyncBuffer]["payload"];
	}

	_globalSyncBuffer.kill();
	_globalSyncBuffer.lock_dec();

	string iterator_dummy = "";

	while(iterator_dummy = dummy[iterator_dummy].nextSibling(), iterator_dummy != "") { 
		if(time_measurement_trigger_to_publish) { 
			if(synchronisation_counter == 0){
				timepoint_first_synchronisation = steady_clock::now();
			}

			synchronisation_counter += 1;

			string payload = (string)dummy[iterator_dummy]["payload"];

			int64_t start_duration_count = stoll(payload, NULL, 10);

			time_log_global_trigger_to_publish << get_time_difference_in_nano(start_duration_count) << endl;

			if(synchronisation_counter == synchronisation_counter_maximum){
				print_time_difference_first_to_last_synchronisation();
				synchronisation_counter = 0;
			}
		}
		else {
			publish_mqtt_message((string)dummy[iterator_dummy]["topic"], (string)dummy[iterator_dummy]["payload"]); 
		}

	}
	
	dummy.kill();
	return MOSQ_ERR_SUCCESS;
}

int mq_messages_per_tick = 0;

int receive_and_publish_mq_messages() 
{
	char buffer[mq_attributes.mq_msgsize + 1];

	for (int i = 0; i < max_mq_receive_per_tick; i++) {
		if(mq_receive(mq_descriptor, buffer, mq_attributes.mq_msgsize + 1, NULL) == -1) { 
			cout << mq_messages_per_tick << endl;
			mq_messages_per_tick = 0;
			return MOSQ_ERR_SUCCESS;
		}

		mq_messages_per_tick += 1;

		char* topic = strtok(buffer, " ");
		char* payload = strtok(NULL, " ");

		if(time_measurement_trigger_to_publish) {
			if(synchronisation_counter == 0){
				timepoint_first_synchronisation = steady_clock::now();
			}

			synchronisation_counter += 1;

			int64_t start_duration_count = strtoll(payload, NULL, 10);
			
			time_log_mq_trigger_to_publish << get_time_difference_in_nano(start_duration_count) << endl;

			if(synchronisation_counter == synchronisation_counter_maximum) {
				print_time_difference_first_to_last_synchronisation();		
				synchronisation_counter = 0;
			}
		}
		else {
			publish_mqtt_message(topic, payload);
		}
	}

	cout << mq_messages_per_tick << endl;
	mq_messages_per_tick = 0;

	return MOSQ_ERR_SUCCESS;
}

// Helfer Funktionen
bool publish_mqtt_message(string topic, string payload) 
{
	int result = mosquitto_broker_publish_copy( 
		NULL,
		topic.c_str(),
		strlen(payload.c_str()),
		payload.c_str(),
		QOS,
		RETAIN,
		PROPERTIES 
	);

	return (result == MOSQ_ERR_SUCCESS);
}

bool publish_mqtt_message(string topic, Json::Value &payload) 
{  
	string json_as_string_type = Json::writeString(stream_writer_builder, payload);

	int result = mosquitto_broker_publish_copy( 
		NULL,
		topic.c_str(),
		strlen(json_as_string_type.c_str()), 
		json_as_string_type.c_str(),
		QOS,
		RETAIN,
		PROPERTIES
	);

	return (result == MOSQ_ERR_SUCCESS);
}

int64_t get_time_difference_in_nano(int64_t start_duration_count)
{
	steady_clock::time_point stop_point = steady_clock::now();
	duration<int64_t, nano> stop_duration = duration_cast<duration<int64_t, nano>>(stop_point.time_since_epoch()); 

	return stop_duration.count() - start_duration_count;
}

void print_time_difference_first_to_last_synchronisation()
{
	steady_clock::time_point timepoint_last_synchronisation = steady_clock::now(); 
	duration<int64_t, nano> time_difference_first_to_last_synchronisation = timepoint_last_synchronisation - timepoint_first_synchronisation;
	cout << "Time difference first to last synchronisation: " << time_difference_first_to_last_synchronisation.count() << endl;
}
