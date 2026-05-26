package main

import (
	"bufio"
	"encoding/binary"
	"encoding/csv"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	pb "termchat/build/relwithdebinfo/proto/go"

	"google.golang.org/protobuf/proto"
)

const password = "password"

type Scenario struct {
	Name              string `json:"name"`
	ServerAddr        string `json:"server_addr"`
	Username          string `json:"username"`
	ReceiverID        uint64 `json:"receiver_id"`
	TotalMessages     int    `json:"total_messages"`
	WarmupMessages    int    `json:"warmup_messages"`
	PayloadBytes      int    `json:"payload_bytes"`
	Pattern           string `json:"pattern"`
	GeneratedProtoDir string `json:"generated_proto_dir"`
	StartedAt         string `json:"started_at"`
}

type LatencyRecord struct {
	Seq       uint64
	MsgID     uint64
	SendUnix  int64
	AckUnix   int64
	LatencyUS int64
	Status    string
	Success   bool
	Error     string
}

type Summary struct {
	Scenario          string             `json:"scenario"`
	Valid             bool               `json:"valid"`
	FatalError        string             `json:"fatal_error,omitempty"`
	FirstFailureSeq   uint64             `json:"first_failure_seq,omitempty"`
	RequestedMessages int                `json:"requested_messages"`
	CompletedMessages int                `json:"completed_messages"`
	Success           int                `json:"success"`
	Failed            int                `json:"failed"`
	SkippedPushes     int                `json:"skipped_pushes"`
	DurationMS        int64              `json:"duration_ms"`
	AttemptedQPS      float64            `json:"attempted_qps"`
	SuccessQPS        float64            `json:"success_qps"`
	LatencyMS         map[string]float64 `json:"latency_ms"`
	Errors            map[string]int     `json:"errors"`
	StartedAt         string             `json:"started_at"`
	FinishedAt        string             `json:"finished_at"`
}

type BenchClient struct {
	rw            *bufio.ReadWriter
	nextSeq       uint64
	skippedPushes int
}

func main() {
	addr := flag.String("addr", "127.0.0.1:1316", "server address")
	totalMessages := flag.Int("n", 10000, "total messages to send")
	warmupMessages := flag.Int("warmup", 100, "warmup messages before measurement")
	username := flag.String("username", "bench_baseline", "benchmark username")
	receiverID := flag.Uint64("receiver", 2, "P2P receiver user id")
	payloadBytes := flag.Int("payload", 256, "payload size in bytes")
	scenarioName := flag.String("scenario", "single_conn_baseline", "scenario name")
	outDir := flag.String("out", "", "output directory, defaults to benchmark-results/<scenario>")
	flag.Parse()

	startedAt := time.Now().UTC()
	scenario := Scenario{
		Name:              *scenarioName,
		ServerAddr:        *addr,
		Username:          *username,
		ReceiverID:        *receiverID,
		TotalMessages:     *totalMessages,
		WarmupMessages:    *warmupMessages,
		PayloadBytes:      *payloadBytes,
		Pattern:           "single_connection_sequential",
		GeneratedProtoDir: "build/debug/proto/go",
		StartedAt:         startedAt.Format(time.RFC3339Nano),
	}

	if *outDir == "" {
		safeName := strings.NewReplacer(" ", "_", "/", "_").Replace(strings.TrimSpace(*scenarioName))
		if safeName == "" {
			safeName = "benchmark"
		}
		*outDir = filepath.Join("benchmark-results", safeName)
	}
	if err := os.MkdirAll(*outDir, 0o755); err != nil {
		log.Fatalf("create output directory failed: %v", err)
	}
	if err := writeJSON(filepath.Join(*outDir, "scenario.json"), scenario); err != nil {
		log.Fatalf("write scenario failed: %v", err)
	}

	fmt.Printf("=== TermChat Go Benchmark ===\n")
	fmt.Printf("Target: %s, messages: %d, output: %s\n", *addr, *totalMessages, *outDir)

	conn, err := net.Dial("tcp", *addr)
	if err != nil {
		log.Fatalf("connect failed: %v", err)
	}
	defer conn.Close()

	client := &BenchClient{
		rw:      bufio.NewReadWriter(bufio.NewReader(conn), bufio.NewWriter(conn)),
		nextSeq: 1,
	}
	if err := client.handshake(*username); err != nil {
		log.Fatalf("handshake failed: %v", err)
	}

	payload := makePayload(*payloadBytes)
	for i := 0; i < *warmupMessages; i++ {
		if _, err := client.sendMeasuredP2P(*receiverID, payload); err != nil {
			log.Fatalf("warmup failed at %d: %v", i, err)
		}
	}
	fmt.Println("Warmup completed")

	records := make([]LatencyRecord, 0, *totalMessages)
	errorsByType := map[string]int{}
	valid := true
	fatalError := ""
	var firstFailureSeq uint64
	benchStart := time.Now()
	for i := 0; i < *totalMessages; i++ {
		record, err := client.sendMeasuredP2P(*receiverID, payload)
		if err != nil {
			errorsByType[classifyError(err)]++
			record.Error = err.Error()
			record.Success = false
			if isFatalConnectionError(err) {
				valid = false
				fatalError = err.Error()
				firstFailureSeq = record.Seq
				records = append(records, record)
				break
			}
		}
		records = append(records, record)
	}
	duration := time.Since(benchStart)

	summary := buildSummary(scenario.Name, *totalMessages, records, errorsByType, client.skippedPushes, duration,
		startedAt, time.Now().UTC(), valid, fatalError, firstFailureSeq)
	if err := writeJSON(filepath.Join(*outDir, "summary.json"), summary); err != nil {
		log.Fatalf("write summary failed: %v", err)
	}
	if err := writeLatencyCSV(filepath.Join(*outDir, "latency.csv"), records); err != nil {
		log.Fatalf("write latency csv failed: %v", err)
	}
	if err := writeReport(filepath.Join(*outDir, "report.md"), scenario, summary); err != nil {
		log.Fatalf("write report failed: %v", err)
	}

	fmt.Printf("\n=== Results ===\n")
	fmt.Printf("Valid: %t\n", summary.Valid)
	if summary.FatalError != "" {
		fmt.Printf("Fatal error at seq %d: %s\n", summary.FirstFailureSeq, summary.FatalError)
	}
	fmt.Printf("Success: %d/%d completed, requested=%d\n", summary.Success, summary.CompletedMessages, summary.RequestedMessages)
	fmt.Printf("Attempted QPS: %.2f, Success QPS: %.2f\n", summary.AttemptedQPS, summary.SuccessQPS)
	fmt.Printf("p50/p95/p99/p999 latency: %.3f / %.3f / %.3f / %.3f ms\n",
		summary.LatencyMS["p50"], summary.LatencyMS["p95"], summary.LatencyMS["p99"], summary.LatencyMS["p999"])
	fmt.Printf("Results written to %s\n", *outDir)
}

func (c *BenchClient) next() uint64 {
	seq := c.nextSeq
	c.nextSeq++
	return seq
}

func (c *BenchClient) handshake(username string) error {
	registerSeq := c.next()
	registerReq := &pb.Envelope{
		Seq:       registerSeq,
		Cmd:       pb.CommandType_CMD_REGISTER_REQ,
		Timestamp: time.Now().Unix(),
		Payload: &pb.Envelope_RegisterReq{
			RegisterReq: &pb.RegisterReq{Username: username, Password: password},
		},
	}
	if err := sendPacket(c.rw, registerReq); err != nil {
		return err
	}
	registerResp, err := c.readExpected(registerSeq, pb.CommandType_CMD_REGISTER_RES)
	if err != nil {
		return fmt.Errorf("register response failed: %w", err)
	}
	if resp := registerResp.GetRegisterRes(); resp != nil && !resp.GetSuccess() {
		fmt.Printf("Register skipped: %s\n", resp.GetErrorMsg())
	}

	loginSeq := c.next()
	loginReq := &pb.Envelope{
		Seq:       loginSeq,
		Cmd:       pb.CommandType_CMD_LOGIN_REQ,
		Timestamp: time.Now().Unix(),
		Payload: &pb.Envelope_LoginReq{
			LoginReq: &pb.LoginReq{Username: username, Password: password},
		},
	}
	if err := sendPacket(c.rw, loginReq); err != nil {
		return err
	}
	loginResp, err := c.readExpected(loginSeq, pb.CommandType_CMD_LOGIN_RES)
	if err != nil {
		return fmt.Errorf("login response failed: %w", err)
	}
	if resp := loginResp.GetLoginRes(); resp == nil || !resp.GetSuccess() {
		if resp == nil {
			return fmt.Errorf("login failed: missing payload")
		}
		return fmt.Errorf("login failed: %s", resp.GetErrorMsg())
	}
	return nil
}

func (c *BenchClient) sendMeasuredP2P(receiverID uint64, content []byte) (LatencyRecord, error) {
	seq := c.next()
	sendAt := time.Now()
	env := &pb.Envelope{
		Seq:       seq,
		Cmd:       pb.CommandType_CMD_P2P_MSG_REQ,
		Timestamp: sendAt.Unix(),
		Payload: &pb.Envelope_P2PMsgReq{
			P2PMsgReq: &pb.P2PMessage{
				ReceiverId:  receiverID,
				Content:     content,
				Timestamp:   sendAt.Unix(),
				ClientMsgId: makeClientMsgID(seq, sendAt),
			},
		},
	}

	record := LatencyRecord{Seq: seq, SendUnix: sendAt.UnixNano()}
	if err := sendPacket(c.rw, env); err != nil {
		record.AckUnix = time.Now().UnixNano()
		record.LatencyUS = time.Since(sendAt).Microseconds()
		return record, err
	}

	respEnv, err := c.readExpected(seq, pb.CommandType_CMD_MSG_ACK)
	ackAt := time.Now()
	record.AckUnix = ackAt.UnixNano()
	record.LatencyUS = ackAt.Sub(sendAt).Microseconds()
	if err != nil {
		return record, err
	}

	ack := respEnv.GetMsgAck()
	if ack == nil {
		return record, fmt.Errorf("missing message ack")
	}
	record.MsgID = ack.GetMsgId()
	record.Status = ack.GetStatus().String()
	record.Success = ack.GetSuccess()
	if !ack.GetSuccess() {
		record.Error = ack.GetErrorMsg()
		return record, fmt.Errorf("message ack failed: %s", ack.GetErrorMsg())
	}
	return record, nil
}

func (c *BenchClient) readExpected(seq uint64, cmd pb.CommandType) (*pb.Envelope, error) {
	for {
		env, err := readPacket(c.rw)
		if err != nil {
			return nil, err
		}
		if env.GetSeq() == seq && env.GetCmd() == cmd {
			return env, nil
		}
		if env.GetSeq() == 0 {
			c.skippedPushes++
			continue
		}
		return nil, fmt.Errorf("unexpected response: seq=%d cmd=%s, expected seq=%d cmd=%s",
			env.GetSeq(), env.GetCmd().String(), seq, cmd.String())
	}
}

func sendPacket(rw *bufio.ReadWriter, env *pb.Envelope) error {
	data, err := proto.Marshal(env)
	if err != nil {
		return err
	}
	lenBuf := make([]byte, 4)
	binary.BigEndian.PutUint32(lenBuf, uint32(len(data)))
	if _, err := rw.Write(lenBuf); err != nil {
		return err
	}
	if _, err := rw.Write(data); err != nil {
		return err
	}
	return rw.Flush()
}

func readPacket(rw *bufio.ReadWriter) (*pb.Envelope, error) {
	lenBuf := make([]byte, 4)
	if _, err := io.ReadFull(rw, lenBuf); err != nil {
		return nil, fmt.Errorf("read length failed: %w", err)
	}
	msgLen := binary.BigEndian.Uint32(lenBuf)
	if msgLen > 10*1024*1024 {
		return nil, fmt.Errorf("message too large: %d", msgLen)
	}
	data := make([]byte, msgLen)
	if _, err := io.ReadFull(rw, data); err != nil {
		return nil, fmt.Errorf("read data failed: %w", err)
	}
	env := &pb.Envelope{}
	if err := proto.Unmarshal(data, env); err != nil {
		return nil, fmt.Errorf("unmarshal failed: %w", err)
	}
	return env, nil
}

func buildSummary(scenario string, requestedMessages int, records []LatencyRecord, errors map[string]int,
	skippedPushes int, duration time.Duration, startedAt time.Time, finishedAt time.Time, valid bool,
	fatalError string, firstFailureSeq uint64) Summary {
	success := 0
	latencies := make([]int64, 0, len(records))
	firstSuccessSend := int64(0)
	lastSuccessAck := int64(0)
	for _, record := range records {
		if record.Success {
			success++
			if firstSuccessSend == 0 {
				firstSuccessSend = record.SendUnix
			}
			lastSuccessAck = record.AckUnix
			latencies = append(latencies, record.LatencyUS)
		}
	}
	failed := len(records) - success
	attemptedQPS := 0.0
	if duration > 0 {
		attemptedQPS = float64(len(records)) / duration.Seconds()
	}
	successQPS := 0.0
	if success > 0 && lastSuccessAck > firstSuccessSend {
		successWindowSeconds := float64(lastSuccessAck-firstSuccessSend) / float64(time.Second)
		successQPS = float64(success) / successWindowSeconds
	}
	return Summary{
		Scenario:          scenario,
		Valid:             valid && failed == 0 && len(records) == requestedMessages,
		FatalError:        fatalError,
		FirstFailureSeq:   firstFailureSeq,
		RequestedMessages: requestedMessages,
		CompletedMessages: len(records),
		Success:           success,
		Failed:            failed,
		SkippedPushes:     skippedPushes,
		DurationMS:        duration.Milliseconds(),
		AttemptedQPS:      attemptedQPS,
		SuccessQPS:        successQPS,
		LatencyMS:         latencyStatsMS(latencies),
		Errors:            errors,
		StartedAt:         startedAt.Format(time.RFC3339Nano),
		FinishedAt:        finishedAt.Format(time.RFC3339Nano),
	}
}

func latencyStatsMS(values []int64) map[string]float64 {
	stats := map[string]float64{"avg": 0, "min": 0, "p50": 0, "p95": 0, "p99": 0, "p999": 0, "max": 0}
	if len(values) == 0 {
		return stats
	}
	sort.Slice(values, func(i, j int) bool { return values[i] < values[j] })
	var total int64
	for _, value := range values {
		total += value
	}
	stats["avg"] = microsToMillis(float64(total) / float64(len(values)))
	stats["min"] = microsToMillis(float64(values[0]))
	stats["p50"] = microsToMillis(float64(percentile(values, 0.50)))
	stats["p95"] = microsToMillis(float64(percentile(values, 0.95)))
	stats["p99"] = microsToMillis(float64(percentile(values, 0.99)))
	stats["p999"] = microsToMillis(float64(percentile(values, 0.999)))
	stats["max"] = microsToMillis(float64(values[len(values)-1]))
	return stats
}

func percentile(sorted []int64, p float64) int64 {
	if len(sorted) == 0 {
		return 0
	}
	index := int(p*float64(len(sorted)-1) + 0.5)
	if index < 0 {
		index = 0
	}
	if index >= len(sorted) {
		index = len(sorted) - 1
	}
	return sorted[index]
}

func writeLatencyCSV(path string, records []LatencyRecord) error {
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	defer file.Close()
	writer := csv.NewWriter(file)
	defer writer.Flush()

	if err := writer.Write([]string{"seq", "msg_id", "send_unix_ns", "ack_unix_ns", "latency_us", "status", "success", "error"}); err != nil {
		return err
	}
	for _, record := range records {
		if err := writer.Write([]string{
			fmt.Sprintf("%d", record.Seq),
			fmt.Sprintf("%d", record.MsgID),
			fmt.Sprintf("%d", record.SendUnix),
			fmt.Sprintf("%d", record.AckUnix),
			fmt.Sprintf("%d", record.LatencyUS),
			record.Status,
			fmt.Sprintf("%t", record.Success),
			record.Error,
		}); err != nil {
			return err
		}
	}
	return writer.Error()
}

func writeReport(path string, scenario Scenario, summary Summary) error {
	content := fmt.Sprintf(`# %s

## Scenario

- Server: %s
- Messages: %d
- Payload: %d bytes
- Pattern: %s

## Summary

- Success: %d/%d
- Failed: %d
- Valid: %t
- Fatal error: %s
- Attempted QPS: %.2f
- Success QPS: %.2f
- p50: %.3f ms
- p95: %.3f ms
- p99: %.3f ms
- p999: %.3f ms
- Max: %.3f ms
- Skipped async pushes: %d
`,
		scenario.Name,
		scenario.ServerAddr,
		scenario.TotalMessages,
		scenario.PayloadBytes,
		scenario.Pattern,
		summary.Success,
		summary.CompletedMessages,
		summary.Failed,
		summary.Valid,
		summary.FatalError,
		summary.AttemptedQPS,
		summary.SuccessQPS,
		summary.LatencyMS["p50"],
		summary.LatencyMS["p95"],
		summary.LatencyMS["p99"],
		summary.LatencyMS["p999"],
		summary.LatencyMS["max"],
		summary.SkippedPushes,
	)
	return os.WriteFile(path, []byte(content), 0o644)
}

func writeJSON(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	return os.WriteFile(path, data, 0o644)
}

func makePayload(size int) []byte {
	if size <= 0 {
		size = 1
	}
	payload := make([]byte, size)
	for i := range payload {
		payload[i] = byte('a' + (i % 26))
	}
	return payload
}

func makeClientMsgID(seq uint64, t time.Time) uint64 {
	return (uint64(t.UnixMilli()) << 20) | (seq & 0xFFFFF)
}

func microsToMillis(value float64) float64 {
	return value / 1000.0
}

func classifyError(err error) string {
	if err == nil {
		return ""
	}
	message := err.Error()
	switch {
	case strings.Contains(message, "timeout"):
		return "timeout"
	case strings.Contains(message, "connect"):
		return "connect"
	case strings.Contains(message, "ack failed"):
		return "ack_failed"
	case strings.Contains(message, "read"):
		return "read"
	case strings.Contains(message, "write"):
		return "write"
	default:
		return "unknown"
	}
}

func isFatalConnectionError(err error) bool {
	if err == nil {
		return false
	}
	message := strings.ToLower(err.Error())
	return strings.Contains(message, "broken pipe") ||
		strings.Contains(message, "connection reset") ||
		strings.Contains(message, "connection refused") ||
		strings.Contains(message, "use of closed network connection") ||
		strings.Contains(message, "read length failed: eof") ||
		strings.Contains(message, "read data failed: eof")
}
