import socket
import struct
import sys
import os
import time
import unittest

current_dir = os.path.dirname(os.path.abspath(__file__))
project_root = os.path.abspath(os.path.join(current_dir, '..'))

proto_paths = [
    os.path.join(project_root, 'build', 'debug', 'proto', 'python'),
    os.path.join(project_root, 'build', 'release', 'proto', 'python'),
    os.path.join(project_root, 'build', 'relwithdebinfo', 'proto', 'python'),
    os.path.join(project_root, 'build', 'macos-debug', 'proto', 'python')
]

for p in proto_paths:
    if os.path.isdir(p):
        sys.path.append(p)

try:
    import protocol_pb2
    import auth_service_pb2
    import message_service_pb2
except ImportError:
    print("[!] Protobuf modules not found.")
    sys.exit(1)

class TestP2PMessage(unittest.TestCase):
    HOST = '127.0.0.1'
    PORT = 1316

    def _create_socket(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((self.HOST, self.PORT))
        return sock

    def _send_msg(self, sock, envelope):
        serialized_data = envelope.SerializeToString()
        msg_len = len(serialized_data)
        header = struct.pack('>I', msg_len)
        sock.sendall(header + serialized_data)

    def _recv_msg(self, sock, timeout=2.0) -> "protocol_pb2.Envelope":
        sock.settimeout(timeout)
        try:
            header_data = sock.recv(4)
            if len(header_data) < 4:
                self.fail("Failed to read response header")
            
            resp_len = struct.unpack('>I', header_data)[0]
            
            resp_data = b''
            while len(resp_data) < resp_len:
                packet = sock.recv(resp_len - len(resp_data))
                if not packet:
                    self.fail("Connection closed while reading response body")
                resp_data += packet
                
            envelope = protocol_pb2.Envelope()
            envelope.ParseFromString(resp_data)
            return envelope
        except socket.timeout as exc:
            raise AssertionError("Timed out while waiting for response") from exc
        except Exception as e:
            raise AssertionError(f"Error receiving response: {e}") from e

    def _register_and_login(self, username, password):
        sock = self._create_socket()

        # 1. Register
        req = protocol_pb2.Envelope()
        req.seq = int(time.time() * 1000)
        req.cmd = protocol_pb2.CMD_REGISTER_REQ
        req.register_req.username = username
        req.register_req.password = password
        self._send_msg(sock, req)
        
        resp = self._recv_msg(sock)
        self.assertEqual(resp.cmd, protocol_pb2.CMD_REGISTER_RES)

        # 2. Login
        req = protocol_pb2.Envelope()
        req.seq = int(time.time() * 1000) + 1
        req.cmd = protocol_pb2.CMD_LOGIN_REQ
        req.login_req.username = username
        req.login_req.password = password
        self._send_msg(sock, req)

        resp = self._recv_msg(sock)
        self.assertEqual(resp.cmd, protocol_pb2.CMD_LOGIN_RES)
        self.assertTrue(resp.login_res.success)
        
        return sock, resp.login_res.user_info.user_id

    def test_p2p_message_flow(self):
        # 1. Setup two users: Alice and Bob
        ts = int(time.time())
        user_a = f"alice_{ts}"
        user_b = f"bob_{ts}"
        
        sock_a, id_a = self._register_and_login(user_a, "password123")
        sock_b, id_b = self._register_and_login(user_b, "password123")
        
        print(f"\nUser A ({user_a}) ID: {id_a}")
        print(f"User B ({user_b}) ID: {id_b}")

        # 2. A sends message to B
        msg_content = "Hello Bob, this is Alice!"
        
        envelope = protocol_pb2.Envelope()
        envelope.seq = 100
        envelope.cmd = protocol_pb2.CMD_P2P_MSG_REQ
        envelope.timestamp = int(time.time())
        
        p2p_msg = envelope.p2p_msg_req
        p2p_msg.receiver_id = id_b
        p2p_msg.content_type = message_service_pb2.CONTENT_TEXT
        p2p_msg.content = msg_content.encode('utf-8')
        p2p_msg.timestamp = int(time.time())

        self._send_msg(sock_a, envelope)
        print(f"A sending message to B: {msg_content}")

        # 3. A expects ACK
        ack_env = self._recv_msg(sock_a)
        self.assertEqual(ack_env.cmd, protocol_pb2.CMD_MSG_ACK)
        self.assertTrue(ack_env.msg_ack.success, f"Error: {ack_env.msg_ack.error_msg}")
        self.assertEqual(ack_env.msg_ack.status, message_service_pb2.ACK_STATUS_DELIVERED)
        self.assertGreater(ack_env.msg_ack.msg_id, 0)
        msg_id = ack_env.msg_ack.msg_id
        print("A received ACK")

        # 4. B expects PUSH
        push_env = self._recv_msg(sock_b)
        self.assertEqual(push_env.cmd, protocol_pb2.CMD_P2P_MSG_PUSH)
        
        push_msg = push_env.p2p_msg_push
        self.assertEqual(push_msg.msg_id, msg_id)
        self.assertEqual(push_msg.sender_id, id_a)
        self.assertEqual(push_msg.receiver_id, id_b)
        self.assertEqual(push_msg.content.decode('utf-8'), msg_content)
        print("B received PUSH")

        # 5. B replies to A
        reply_content = "Hi Alice! Got it."
        
        envelope = protocol_pb2.Envelope()
        envelope.seq = 101
        envelope.cmd = protocol_pb2.CMD_P2P_MSG_REQ
        envelope.timestamp = int(time.time())
        
        p2p_msg = envelope.p2p_msg_req
        p2p_msg.receiver_id = id_a
        p2p_msg.content_type = message_service_pb2.CONTENT_TEXT
        p2p_msg.content = reply_content.encode('utf-8')
        p2p_msg.timestamp = int(time.time())

        self._send_msg(sock_b, envelope)
        print(f"B sending reply to A: {reply_content}")

        # 6. B expects ACK
        ack_env_b = self._recv_msg(sock_b)
        self.assertEqual(ack_env_b.cmd, protocol_pb2.CMD_MSG_ACK)
        self.assertTrue(ack_env_b.msg_ack.success)
        self.assertEqual(ack_env_b.msg_ack.status, message_service_pb2.ACK_STATUS_DELIVERED)
        self.assertGreater(ack_env_b.msg_ack.msg_id, 0)
        reply_msg_id = ack_env_b.msg_ack.msg_id
        print("B received ACK")

        # 7. A expects PUSH
        push_env_a = self._recv_msg(sock_a)
        self.assertEqual(push_env_a.cmd, protocol_pb2.CMD_P2P_MSG_PUSH)
        self.assertEqual(push_env_a.p2p_msg_push.msg_id, reply_msg_id)
        self.assertEqual(push_env_a.p2p_msg_push.content.decode('utf-8'), reply_content)
        print("A received PUSH")

        # Cleanup
        sock_a.close()
        sock_b.close()

if __name__ == '__main__':
    unittest.main()
