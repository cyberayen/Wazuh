#!/usr/bin/env python

# Created by Wazuh, Inc. <info@wazuh.com>.
# This program is a free software; you can redistribute it and/or modify it under the terms of GPLv2

import logging
import threading
import time

from wazuh.cluster.cluster import get_cluster_items, _update_file, decompress_files, get_files_status, compress_files
from wazuh.exception import WazuhException
from wazuh import common



def compare_files(good_files, check_files):

    missing_files = set(good_files.keys()) - set(check_files.keys())
    extra_files = set(check_files.keys()) - set(good_files.keys())

    shared_files = {name: {'cluster_item_key': data['cluster_item_key']} for name, data in good_files.iteritems() if name in check_files and data['md5'] != check_files[name]['md5']}

    if not missing_files:
        missing_files = {}
    else:
        missing_files = {missing_file: {'cluster_item_key': good_files[missing_file]['cluster_item_key']} for missing_file in missing_files }

    if not extra_files:
        extra_files = {}
    else:
        extra_files = {extra_file: {'cluster_item_key': check_files[extra_file]['cluster_item_key']} for extra_file in extra_files }

    return {'missing': missing_files, 'extra': extra_files, 'shared': shared_files}


def update_client_files_in_master(json_file, files_to_update):
    cluster_items = get_cluster_items()

    try:

        for file_name, data in json_file.iteritems():
            # Full path
            file_path = common.ossec_path + file_name

            # Cluster items information: write mode and umask
            cluster_item_key = data['cluster_item_key']
            w_mode = cluster_items[cluster_item_key]['write_mode']
            umask = int(cluster_items[cluster_item_key]['umask'], base=0)

            # File content and time
            file_data = files_to_update[file_name]['data']
            file_time = files_to_update[file_name]['time']

            _update_file(fullpath=file_path, new_content=file_data, umask_int=umask, mtime=file_time, w_mode=w_mode, whoami='master')

    except Exception as e:
        print(str(e))
        raise e


def force_clients_to_start_sync(node_list=None):
    nodes_response = {}

    config_cluster = read_config()
    if not config_cluster:
        raise WazuhException(3000, "No config found")

    # Get nodes
    all_nodes = get_nodes()['items']

    for node in all_nodes:
        if node['type'] == 'master':
            continue

        if node_list and node['node'] not in node_list:
            continue

        if node['status'] == 'connected':

            error, response = send_request( host=node['url'],
                                    port=config_cluster["port"],
                                    key=config_cluster['key'],
                                    connection_timeout=100, #int(config_cluster['connection_timeout']),
                                    socket_timeout=100, #int(config_cluster['socket_timeout']),
                                    data="force_sync {0}".format('-'*(common.cluster_protocol_plain_size - len("force_sync ")))
            )

            nodes_response[node['node']] = response['data']
        else:
            nodes_response[node['node']] = 'Disconnected: {0}'.format(node['url'])

    return nodes_response



from wazuh.cluster.communication import Server, ServerHandler, Handler


class MasterManagerHandler(ServerHandler):

    def __init__(self, sock, server, map, addr=None):
        ServerHandler.__init__(self, sock, server, map, addr)
        self.manager = server

    def process_request(self, command, data):
        logging.debug("[Master] Request received: '{0}'.".format(command))

        if command == 'echo-c':
            return 'ok-c ', data.decode()
        elif command == 'req_sync_m_c':
            return 'ack', 'Starting sync from master'
        elif command == 'sync_c_m':
            mcf_thread = MasterProcessClientFiles(self, self.get_client(), data)
            mcf_thread.start()
            return 'ack', 'Sync: Thanks client, Im going to do it'  # ToDo
        else:
            return ServerHandler.process_request(self, command, data)

    @staticmethod
    def process_response(response):
        # FixMe: Move this line to communications
        answer, payload = Handler.split_data(response)

        logging.debug("[Master] Response received: '{0}'.".format(answer))

        response_data = None

        if answer == 'ok-m':  # test
            response_data = '[response_only_for_master] Client answered: {}.'.format(payload)
        else:
            response_data = ServerHandler.process_response(response)

        return response_data

    def process_files_from_client(self, client_name, data_received):
        sync_result = False

        logging.info("[Master] [Sync process c->m] [{0}]: Start.".format(client_name))

        client_data = decompress_files(data_received)

        # Extract recevied data
        logging.info("[Master] [Sync process c->m] [{0}] [STEP 1]: Analyzing received files.".format(client_name))

        master_files_from_client = {}
        client_files = {}
        for key in client_data:
            if key == 'cluster_control.json':
                json_file = json.loads(client_data['cluster_control.json']['data'])
                master_files_from_client = json_file['master_files']
                client_files_json = json_file['client_files']
            else:
                full_path_key = key.replace('files/', '/')
                client_files[full_path_key] = client_data[key]

        # Get master files
        master_files = get_files_status('master')

        # Compare
        client_files_ko = compare_files(master_files, master_files_from_client)

        logging.info("[Master] [Sync process c->m] [{0}] [STEP 2]: Updating manager files.".format(client_name))

        # Update files
        update_client_files_in_master(client_files_json, client_files)

        # Compress data: master files (only KO shared and missing)
        logging.info("[Master] [Sync process c->m] [{0}] [STEP 3]: Compressing KO files for client.".format(client_name))

        master_files_paths = [item for item in client_files_ko['shared']]
        master_files_paths.extend([item for item in client_files_ko['missing']])

        compressed_data = compress_files('master', master_files_paths, client_files_ko)

        logging.info("[Master] [Sync process c->m] [{0}]: End. Sending KO files to client.".format(client_name))

        response = self.manager.send_request(client_name, 'sync_m_c', compressed_data)
        processed_response = self.process_response(response)
        if processed_response:
            sync_result = True
            logging.info("[Master] [Sync process m->c] [{0}] [STEP 3]: {1}".format(client_name, processed_response))
        else:
            logging.error("[Master] [Sync process m->c] [{0}] [STEP 3]: Client reported an error receiving files.".format(client_name))

        # Send KO files
        return sync_result



class MasterManager(Server):

    def __init__(self, cluster_config):
        Server.__init__(self, cluster_config['bind_addr'],
            cluster_config['port'], MasterManagerHandler)

        logging.info("[Master] Listening.")

        self.config = cluster_config
        self.handler = MasterManagerHandler

    def req_file_status_to_clients(self):
        nodes_file = {}
        for node_name, response in self.send_request_broadcast(command = 'file_status'):
            logging.debug("Response from {}: {}".format(node_name, response))

        return 'ok', "File status"

#
# Master threads
#
class MasterKeepAliveThread(threading.Thread):

    def __init__(self, manager):
        threading.Thread.__init__(self)
        self.daemon = True
        self.running = True
        self.manager = manager


    def run(self):

        while self.running:
            connected_clients = len(self.manager.get_connected_clients())

            if connected_clients > 0:
                logging.debug("[Master] Sending KA to clients ({0}).".format(connected_clients))

                for client_name, response in self.manager.send_request_broadcast('echo-m', 'Keep-alive from master!'):
                    processed_response = self.manager.handler.process_response(response)
                    if processed_response:
                        logging.debug("[Master] KA received from client: '{0}'.".format(client_name))
                    else:
                        logging.error("[Master] KA was not received from client: '{0}'.".format(client_name))

            time.sleep(self.manager.config['ka_interval'])

    def stop(self):
        self.running = False


class MasterProcessClientFiles(threading.Thread):

    def __init__(self, manager_handler, client_name, data):
        threading.Thread.__init__(self)
        self.daemon = True
        self.running = True
        self.manager_handler = manager_handler
        self.client_name = client_name
        self.data = data

    def run(self):

        while self.running:
            logging.debug("[Master-FileThread] Started for {0}.".format(self.client_name))
            self.manager_handler.process_files_from_client(self.client_name, self.data)
            logging.debug("[Master-FileThread] Ended for {0}.".format(self.client_name))
            self.stop()

    def stop(self):
        self.running = False

#
# Internal socket
#
from wazuh.cluster.communication import InternalSocketHandler
import json

class MasterInternalSocketHandler(InternalSocketHandler):
    def __init__(self, sock, manager, map):
        InternalSocketHandler.__init__(self, sock=sock, manager=manager, map=map)

    def process_request(self, command, data):
        logging.debug("[Transport-I] Forwarding request to master of cluster '{0}' - '{1}'".format(command, data))
        serialized_response = ""

        if command == 'req_file_s_c':
            return self.manager.req_file_status_to_clients()

        else:
            split_data = data.split(' ', 1)
            host = split_data[0]
            data = split_data[1] if len(split_data) > 1 else None

            if host == 'all':
                response = list(self.manager.send_request_broadcast(command=command, data=data))
                serialized_response = ['ok', json.dumps({node:data for node,data in response})]
            else:
                response = self.manager.send_request(client_name=host, command=command, data=data)
                if response:
                    serialized_response = response.split(' ', 1)

            return serialized_response
