import socket
import struct
import time
import threading
import sys
import random
import os

sys.path.append(os.path.join(os.path.dirname(__file__), '../build/relwithdebinfo/proto/python'))
import protocol_pb2
import common_pb2
import auth_service_pb2
import message_service_pb2

HOST = '127.0.0.1'
PORT = 1316
CLIENT_COUNT = 10000
MSGS_PER_CLIENT = 200

def send_packet(sock, cmd, message):
    serialized = message.SerializeToString()
    length = struct.pack('!I', len(serialized))
    sock.sendall(length + serialized)

def read_response(sock):
    header = sock.recv(4)
    if len(header) < 4:
        return None, None
    msg_len = struct.unpack('!I', header)[0]
    
    data = b''
    while len(data) < msg_len:
        packet = sock.recv(msg_len - len(data))
        if not packet:
            break
        data += packet
    
    env = protocol_pb2.Envelope()
    env.ParseFromString(data)
    return env.cmd, env

def client_task(client_id):
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((HOST, PORT))
        
        # 1. Register
        username = f"bench_user_{client_id}"
        password = "password"
        
        reg_req = auth_service_pb2.RegisterReq()
        reg_req.username = username
        reg_req.password = password
        
        env = protocol_pb2.Envelope()
        env.cmd = protocol_pb2.CMD_REGISTER_REQ
        env.register_req.CopyFrom(reg_req)
        send_packet(sock, env.cmd, env)
        read_response(sock)

        # 2. Login
        login_req = auth_service_pb2.LoginReq()
        login_req.username = username
        login_req.password = password
        
        env = protocol_pb2.Envelope()
        env.cmd = protocol_pb2.CMD_LOGIN_REQ
        env.login_req.CopyFrom(login_req)
        send_packet(sock, env.cmd, env)
        cmd, resp = read_response(sock)
        
        if not resp or not resp.login_res.success:
            print(f"Client {client_id} login failed")
            sock.close()
            return

        # 3. Send Messages Loop
        start = time.time()
        for i in range(MSGS_PER_CLIENT):
            msg = message_service_pb2.P2PMessage()
            msg.receiver_id = client_id + 1
            payload = f"Hello world {i} from client {client_id} time {time.time()}"
            msg.content = payload.encode('ascii')
            msg.timestamp = int(time.time())
            
            env = protocol_pb2.Envelope()
            env.cmd = protocol_pb2.CMD_P2P_MSG_REQ
            env.p2p_msg_req.CopyFrom(msg)
            
            send_packet(sock, env.cmd, env)
            
            cmd, resp = read_response(sock)
            
        duration = time.time() - start
        print(f"Client {client_id} finished: {MSGS_PER_CLIENT / duration:.2f} qps")
        
        sock.close()
    except Exception as e:
        print(f"Client {client_id} error: {e}")

if __name__ == "__main__":
    print(f"Starting benchmark: {CLIENT_COUNT} clients, {MSGS_PER_CLIENT} msgs each...")
    threads = []
    start_total = time.time()
    
    for i in range(CLIENT_COUNT):
        t = threading.Thread(target=client_task, args=(i,))
        threads.append(t)
        t.start()
        
    for t in threads:
        t.join()
        
    total_time = time.time() - start_total
    total_reqs = CLIENT_COUNT * MSGS_PER_CLIENT
    print(f"Total QPS: {total_reqs / total_time:.2f}")
