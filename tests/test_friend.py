import socket
import struct
import sys
import os
import time
from typing import Optional

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
    import friend_service_pb2
except ImportError:
    print("[!] Protobuf modules not found. Please run:")
    print("    protoc -I=proto --python_out=proto proto/*.proto")
    print("    OR ensure CMake has generated them in build/debug/proto/python")
    sys.exit(1)


class Client:
    """Helper class to manage a single client connection with session state."""
    
    def __init__(self, host='127.0.0.1', port=1316):
        self.host = host
        self.port = port
        self.sock: Optional[socket.socket] = None
        self.user_id: Optional[int] = None
        self.username: Optional[str] = None
        self.seq = int(time.time() * 1000)
    
    def connect(self):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5.0)
            sock.connect((self.host, self.port))
            self.sock = sock
            return True
        except ConnectionRefusedError:
            print(f"[!] Connection failed. Is the server running on port {self.port}?")
            return False
    
    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None
    
    def next_seq(self):
        self.seq += 1
        return self.seq

    def _socket(self) -> socket.socket:
        if self.sock is None:
            raise RuntimeError("Client is not connected")
        return self.sock
    
    def send_msg(self, envelope):
        serialized_data = envelope.SerializeToString()
        msg_len = len(serialized_data)
        header = struct.pack('>I', msg_len)
        self._socket().sendall(header + serialized_data)
    
    def recv_msg(self):
        try:
            sock = self._socket()
            header_data = sock.recv(4)
            if len(header_data) < 4:
                print("[!] Failed to read response header")
                return None
            
            resp_len = struct.unpack('>I', header_data)[0]
            
            resp_data = b''
            while len(resp_data) < resp_len:
                packet = sock.recv(resp_len - len(resp_data))
                if not packet:
                    break
                resp_data += packet
            
            envelope = protocol_pb2.Envelope()
            envelope.ParseFromString(resp_data)
            return envelope
        except Exception as e:
            print(f"[!] Error receiving: {e}")
            return None
    
    def register(self, username, password):
        """Register a new user."""
        envelope = protocol_pb2.Envelope()
        envelope.seq = self.next_seq()
        envelope.cmd = protocol_pb2.CMD_REGISTER_REQ
        envelope.timestamp = int(time.time())
        
        req = envelope.register_req
        req.username = username
        req.password = password
        
        self.send_msg(envelope)
        resp = self.recv_msg()
        
        if resp and resp.cmd == protocol_pb2.CMD_REGISTER_RES:
            res = resp.register_res
            if res.success:
                self.user_id = res.user_id
                self.username = username
            return res
        return None
    
    def login(self, username, password):
        """Login an existing user."""
        envelope = protocol_pb2.Envelope()
        envelope.seq = self.next_seq()
        envelope.cmd = protocol_pb2.CMD_LOGIN_REQ
        envelope.timestamp = int(time.time())
        
        req = envelope.login_req
        req.username = username
        req.password = password
        
        self.send_msg(envelope)
        resp = self.recv_msg()
        
        if resp and resp.cmd == protocol_pb2.CMD_LOGIN_RES:
            res = resp.login_res
            if res.success:
                self.user_id = res.user_info.user_id
                self.username = res.user_info.username
            return res
        return None
    
    def add_friend(self, receiver_id, verify_msg="Hello, let's be friends!"):
        """Send a friend request."""
        envelope = protocol_pb2.Envelope()
        envelope.seq = self.next_seq()
        envelope.cmd = protocol_pb2.CMD_ADD_FRIEND_REQ
        envelope.timestamp = int(time.time())
        
        req = envelope.add_friend_req
        req.receiver_id = receiver_id
        req.verify_msg = verify_msg
        
        self.send_msg(envelope)
        resp = self.recv_msg()
        
        if resp and resp.cmd == protocol_pb2.CMD_ADD_FRIEND_RES:
            return resp.add_friend_res
        return None
    
    def handle_friend(self, sender_id, accept=True):
        """Accept or reject a friend request."""
        envelope = protocol_pb2.Envelope()
        envelope.seq = self.next_seq()
        envelope.cmd = protocol_pb2.CMD_HANDLE_FRIEND_REQ
        envelope.timestamp = int(time.time())
        
        req = envelope.handle_friend_req
        req.sender_id = sender_id
        req.action = friend_service_pb2.ACTION_ACCEPT if accept else friend_service_pb2.ACTION_REJECT
        
        self.send_msg(envelope)
        resp = self.recv_msg()
        
        if resp and resp.cmd == protocol_pb2.CMD_HANDLE_FRIEND_RES:
            return resp.handle_friend_res
        return None
    
    def get_friend_list(self):
        """Get the friend list."""
        envelope = protocol_pb2.Envelope()
        envelope.seq = self.next_seq()
        envelope.cmd = protocol_pb2.CMD_GET_FRIEND_LIST_REQ
        envelope.timestamp = int(time.time())
        
        # GetFriendListReq has no fields, just set the oneof
        envelope.get_friend_list_req.SetInParent()
        
        self.send_msg(envelope)
        resp = self.recv_msg()
        
        if resp and resp.cmd == protocol_pb2.CMD_GET_FRIEND_LIST_RES:
            return resp.get_friend_list_res
        return None


def test_friend_workflow():
    """Test the complete friend workflow with two users."""
    
    timestamp = int(time.time()) % 100000
    user_a_name = f"alice_{timestamp}"
    user_b_name = f"bob_{timestamp}"
    password = "password123"
    
    print("=" * 60)
    print("Friend Service Test")
    print("=" * 60)
    
    # --- Create and login User A ---
    print(f"\n[1] Creating User A: {user_a_name}")
    client_a = Client()
    if not client_a.connect():
        return False
    
    res = client_a.register(user_a_name, password)
    if res and res.success:
        print(f"    ✅ Registered: user_id={client_a.user_id}")
    else:
        print(f"    ❌ Register failed: {res.error_msg if res else 'No response'}")
        client_a.close()
        return False
    
    res = client_a.login(user_a_name, password)
    if res and res.success:
        print(f"    ✅ Logged in: user_id={client_a.user_id}")
    else:
        print(f"    ❌ Login failed: {res.error_msg if res else 'No response'}")
        client_a.close()
        return False
    
    # --- Create and login User B ---
    print(f"\n[2] Creating User B: {user_b_name}")
    client_b = Client()
    if not client_b.connect():
        client_a.close()
        return False
    
    res = client_b.register(user_b_name, password)
    if res and res.success:
        print(f"    ✅ Registered: user_id={client_b.user_id}")
    else:
        print(f"    ❌ Register failed: {res.error_msg if res else 'No response'}")
        client_a.close()
        client_b.close()
        return False
    
    res = client_b.login(user_b_name, password)
    if res and res.success:
        print(f"    ✅ Logged in: user_id={client_b.user_id}")
    else:
        print(f"    ❌ Login failed: {res.error_msg if res else 'No response'}")
        client_a.close()
        client_b.close()
        return False
    
    # --- User A sends friend request to User B ---
    print(f"\n[3] User A sends friend request to User B")
    res = client_a.add_friend(client_b.user_id, "Hi Bob, let's be friends!")
    if res and res.success:
        print(f"    ✅ Friend request sent successfully")
    else:
        print(f"    ❌ Add friend failed: {res.error_msg if res else 'No response'}")
        client_a.close()
        client_b.close()
        return False
    
    # --- User B receives push notification ---
    print(f"\n[3.1] User B waiting for Friend Request Push")
    push_msg = client_b.recv_msg()
    if push_msg and push_msg.cmd == protocol_pb2.CMD_FRIEND_REQ_PUSH:
        req_push = push_msg.friend_req_push
        print(f"    ✅ User B received Push Notification from {req_push.sender_name}")
        if req_push.sender_id != client_a.user_id:
            print(f"    ❌ Sender ID mismatch: expected {client_a.user_id}, got {req_push.sender_id}")
            return False
    else:
        print(f"    ❌ User B did not receive expected Push Notification (Got: {push_msg.cmd if push_msg else 'None'})")
        return False

    # --- Test duplicate friend request ---
    print(f"\n[4] User A sends duplicate friend request (should fail)")
    res = client_a.add_friend(client_b.user_id)
    if res and not res.success:
        print(f"    ✅ Correctly rejected: {res.error_msg}")
    else:
        print(f"    ⚠️ Unexpected: duplicate request was accepted")
    
    # --- User B accepts friend request ---
    print(f"\n[5] User B accepts friend request from User A")
    res = client_b.handle_friend(client_a.user_id, accept=True)
    if res and res.success:
        print(f"    ✅ Friend request accepted, sender_id={res.sender_id}")
    else:
        print(f"    ❌ Handle friend failed: {res.error_msg if res else 'No response'}")
        client_a.close()
        client_b.close()
        return False
    
    # --- User A receives status push notification ---
    print(f"\n[5.1] User A waiting for Friend Status Push")
    push_msg = client_a.recv_msg()
    if push_msg and push_msg.cmd == protocol_pb2.CMD_FRIEND_STATUS_PUSH:
        status_push = push_msg.friend_status_push
        print(f"    ✅ User A received Status Push: {status_push.receiver_name} {status_push.action}")
        if status_push.receiver_id != client_b.user_id:
            print(f"    ❌ Handler ID mismatch: expected {client_b.user_id}, got {status_push.receiver_id}")
            return False
    else:
        print(f"    ❌ User A did not receive expected Status Push (Got: {push_msg.cmd if push_msg else 'None'})")
        return False

    # --- User A gets friend list ---
    print(f"\n[6] User A gets friend list")
    res = client_a.get_friend_list()
    if res and res.success:
        print(f"    ✅ Friend list retrieved: {len(res.friend_list)} friend(s)")
        for friend in res.friend_list:
            print(f"       - {friend.username} (ID: {friend.user_id})")
        
        # Verify User B is in the list
        friend_ids = [f.user_id for f in res.friend_list]
        if client_b.user_id in friend_ids:
            print(f"    ✅ User B found in User A's friend list")
        else:
            print(f"    ❌ User B NOT found in User A's friend list")
    else:
        print(f"    ❌ Get friend list failed: {res.error_msg if res else 'No response'}")
    
    # --- User B gets friend list ---
    print(f"\n[7] User B gets friend list")
    res = client_b.get_friend_list()
    if res and res.success:
        print(f"    ✅ Friend list retrieved: {len(res.friend_list)} friend(s)")
        for friend in res.friend_list:
            print(f"       - {friend.username} (ID: {friend.user_id})")
        
        # Verify User A is in the list
        friend_ids = [f.user_id for f in res.friend_list]
        if client_a.user_id in friend_ids:
            print(f"    ✅ User A found in User B's friend list")
        else:
            print(f"    ❌ User A NOT found in User B's friend list")
    else:
        print(f"    ❌ Get friend list failed: {res.error_msg if res else 'No response'}")
    
    # --- Cleanup ---
    client_a.close()
    client_b.close()
    
    print("\n" + "=" * 60)
    print("Test completed!")
    print("=" * 60)
    return True


def test_unauthorized_access():
    """Test that friend operations require authentication."""
    
    print("\n" + "=" * 60)
    print("Unauthorized Access Test")
    print("=" * 60)
    
    client = Client()
    if not client.connect():
        return False
    
    # Try to add friend without login
    print("\n[1] Attempting add_friend without login")
    res = client.add_friend(123456789)
    if res is None or not res.success:
        print(f"    ✅ Correctly rejected (not logged in)")
    else:
        print(f"    ❌ Unexpected: request was accepted without login")
    
    # Try to get friend list without login
    print("\n[2] Attempting get_friend_list without login")
    res = client.get_friend_list()
    if res is None or not res.success:
        print(f"    ✅ Correctly rejected (not logged in)")
    else:
        print(f"    ❌ Unexpected: request was accepted without login")
    
    client.close()
    print("\n" + "=" * 60)
    return True


if __name__ == "__main__":
    print("\n🧪 Running Friend Service Tests\n")
    
    test_unauthorized_access()
    test_friend_workflow()
