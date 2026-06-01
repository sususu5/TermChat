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
	"math/rand"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	pb "termchat/build/relwithdebinfo/proto/go"

	"google.golang.org/protobuf/proto"
)

const password = "password"

type Scenario struct {
	Name               string  `json:"name"`
	ServerAddr         string  `json:"server_addr"`
	Username           string  `json:"username"`
	ReceiverID         uint64  `json:"receiver_id"`
	ReceiverMode       string  `json:"receiver_mode"`
	Clients            int     `json:"clients"`
	MessagesPerClient  int     `json:"messages_per_client"`
	TotalMessages      int     `json:"total_messages"`
	WarmupMessages     int     `json:"warmup_messages"`
	PayloadBytes       int     `json:"payload_bytes"`
	Inflight           int     `json:"inflight"`
	ConnectRampSeconds float64 `json:"connect_ramp_seconds,omitempty"`
	DurationSeconds    float64 `json:"duration_seconds,omitempty"`
	RatePerClient      float64 `json:"rate_per_client,omitempty"`
	RateSchedule       string  `json:"rate_schedule,omitempty"`
	RequestTimeoutMS   int64   `json:"request_timeout_ms,omitempty"`
	DrainSeconds       float64 `json:"drain_seconds,omitempty"`
	Mode               string  `json:"mode"`
	Pattern            string  `json:"pattern"`
	GeneratedProtoDir  string  `json:"generated_proto_dir"`
	StartedAt          string  `json:"started_at"`
}

type LatencyRecord struct {
	ClientID  int
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
	Scenario             string             `json:"scenario"`
	Valid                bool               `json:"valid"`
	FatalError           string             `json:"fatal_error,omitempty"`
	FirstFailureSeq      uint64             `json:"first_failure_seq,omitempty"`
	FirstFailureClient   int                `json:"first_failure_client"`
	Clients              int                `json:"clients"`
	MessagesPerClient    int                `json:"messages_per_client"`
	Inflight             int                `json:"inflight"`
	ConnectRampSeconds   float64            `json:"connect_ramp_seconds,omitempty"`
	DurationSeconds      float64            `json:"duration_seconds,omitempty"`
	RatePerClient        float64            `json:"rate_per_client,omitempty"`
	RateSchedule         string             `json:"rate_schedule,omitempty"`
	RequestTimeoutMS     int64              `json:"request_timeout_ms,omitempty"`
	DrainSeconds         float64            `json:"drain_seconds,omitempty"`
	ReceiverMode         string             `json:"receiver_mode"`
	Mode                 string             `json:"mode"`
	RequestedMessages    int                `json:"requested_messages"`
	CompletedMessages    int                `json:"completed_messages"`
	Success              int                `json:"success"`
	Failed               int                `json:"failed"`
	SkippedPushes        int                `json:"skipped_pushes"`
	SkippedStaleAcks     int                `json:"skipped_stale_acks"`
	DurationMS           int64              `json:"duration_ms"`
	MeasurementMS        int64              `json:"measurement_ms"`
	AttemptedQPS         float64            `json:"attempted_qps"`
	SuccessQPS           float64            `json:"success_qps"`
	EndToEndAttemptedQPS float64            `json:"end_to_end_attempted_qps"`
	EndToEndSuccessQPS   float64            `json:"end_to_end_success_qps"`
	LatencyMS            map[string]float64 `json:"latency_ms"`
	Errors               map[string]int     `json:"errors"`
	StartedAt            string             `json:"started_at"`
	FinishedAt           string             `json:"finished_at"`
}

type BenchClient struct {
	clientID         int
	userID           uint64
	conn             net.Conn
	rw               *bufio.ReadWriter
	nextSeq          uint64
	skippedPushes    int
	skippedStaleAcks int
	receiverID       uint64
	receiverMode     string
	receivers        *receiverRegistry
	requestTimeout   time.Duration
	rng              *rand.Rand
}

type receiverRegistry struct {
	mu  sync.RWMutex
	ids []uint64
}

func (r *receiverRegistry) add(id uint64) {
	if id == 0 {
		return
	}
	r.mu.Lock()
	r.ids = append(r.ids, id)
	r.mu.Unlock()
}

func (r *receiverRegistry) snapshot() []uint64 {
	r.mu.RLock()
	defer r.mu.RUnlock()
	ids := make([]uint64, len(r.ids))
	copy(ids, r.ids)
	return ids
}

func main() {
	addr := flag.String("addr", "127.0.0.1:1316", "server address")
	legacyMessages := flag.Int("n", 10000, "legacy per-client message count when -messages-per-client is not set")
	clients := flag.Int("clients", 1, "number of benchmark clients/connections")
	messagesPerClient := flag.Int("messages-per-client", 0, "messages to send per client, defaults to -n")
	durationFlag := flag.Duration("duration", 0, "rate mode duration, for example 120s or 5m")
	ratePerClient := flag.Float64("rate-per-client", 0, "rate mode target messages per second per client")
	rateSchedule := flag.String("rate-schedule", "poisson", "rate mode send schedule: poisson or fixed")
	requestTimeout := flag.Duration("request-timeout", 30*time.Second, "per request ACK timeout")
	drainDuration := flag.Duration("drain", 30*time.Second, "post-measurement connection drain duration before clients close")
	warmupMessages := flag.Int("warmup", 100, "warmup messages per client before measurement")
	username := flag.String("username", "bench_baseline", "benchmark username prefix")
	receiverID := flag.Uint64("receiver", 2, "P2P receiver user id")
	receiverMode := flag.String("receiver-mode", "fixed", "receiver selection mode: fixed or random-online")
	payloadBytes := flag.Int("payload", 256, "payload size in bytes")
	inflight := flag.Int("inflight", 1, "max in-flight P2P requests per benchmark connection")
	connectRamp := flag.Duration("connect-ramp", 0, "spread client connection startup over this duration, for example 30s")
	scenarioName := flag.String("scenario", "single_conn_baseline", "scenario name")
	outDir := flag.String("out", "", "output directory, defaults to benchmark-results/<scenario>")
	flag.Parse()
	if *clients < 1 {
		log.Fatalf("clients must be >= 1")
	}
	if *inflight < 1 {
		log.Fatalf("inflight must be >= 1")
	}
	if *messagesPerClient <= 0 {
		*messagesPerClient = *legacyMessages
	}
	if *messagesPerClient < 1 {
		log.Fatalf("messages-per-client must be >= 1")
	}
	if *warmupMessages < 0 {
		log.Fatalf("warmup must be >= 0")
	}
	if *connectRamp < 0 {
		log.Fatalf("connect-ramp must be >= 0")
	}
	if *receiverMode != "fixed" && *receiverMode != "random-online" {
		log.Fatalf("receiver-mode must be fixed or random-online")
	}
	if *rateSchedule != "poisson" && *rateSchedule != "fixed" {
		log.Fatalf("rate-schedule must be poisson or fixed")
	}
	if *requestTimeout <= 0 {
		log.Fatalf("request-timeout must be > 0")
	}
	if *drainDuration < 0 {
		log.Fatalf("drain must be >= 0")
	}
	rateMode := *durationFlag > 0 || *ratePerClient > 0
	if rateMode {
		if *durationFlag <= 0 {
			log.Fatalf("duration must be > 0 when rate-per-client is set")
		}
		if *ratePerClient <= 0 {
			log.Fatalf("rate-per-client must be > 0 when duration is set")
		}
		if *inflight != 1 {
			log.Fatalf("rate mode currently requires inflight=1")
		}
	}

	startedAt := time.Now().UTC()
	mode := "fixed_messages"
	pattern := "single_connection_sequential"
	if rateMode {
		mode = "rate_limited"
		if *clients > 1 {
			pattern = "multi_connection_rate_limited"
		} else {
			pattern = "single_connection_rate_limited"
		}
	} else if *clients > 1 && *inflight > 1 {
		pattern = "multi_connection_pipelined"
	} else if *clients > 1 {
		pattern = "multi_connection_sequential"
	} else if *inflight > 1 {
		pattern = "single_connection_pipelined"
	}
	totalMessages := *clients * *messagesPerClient
	if rateMode {
		totalMessages = int(float64(*clients) * durationFlag.Seconds() * *ratePerClient)
	}
	scenario := Scenario{
		Name:               *scenarioName,
		ServerAddr:         *addr,
		Username:           *username,
		ReceiverID:         *receiverID,
		ReceiverMode:       *receiverMode,
		Clients:            *clients,
		MessagesPerClient:  *messagesPerClient,
		TotalMessages:      totalMessages,
		WarmupMessages:     *warmupMessages,
		PayloadBytes:       *payloadBytes,
		Inflight:           *inflight,
		ConnectRampSeconds: connectRamp.Seconds(),
		DurationSeconds:    durationFlag.Seconds(),
		RatePerClient:      *ratePerClient,
		RateSchedule:       *rateSchedule,
		RequestTimeoutMS:   requestTimeout.Milliseconds(),
		DrainSeconds:       drainDuration.Seconds(),
		Mode:               mode,
		Pattern:            pattern,
		GeneratedProtoDir:  "build/relwithdebinfo/proto/go",
		StartedAt:          startedAt.Format(time.RFC3339Nano),
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
	if rateMode {
		fmt.Printf("Target: %s, clients: %d, duration: %s, rate/client: %.3f msg/s, inflight: %d, connect-ramp: %s, drain: %s, output: %s\n",
			*addr, *clients, durationFlag.String(), *ratePerClient, *inflight, connectRamp.String(), drainDuration.String(), *outDir)
	} else {
		fmt.Printf("Target: %s, clients: %d, messages/client: %d, total: %d, inflight: %d, connect-ramp: %s, drain: %s, output: %s\n",
			*addr, *clients, *messagesPerClient, totalMessages, *inflight, connectRamp.String(), drainDuration.String(), *outDir)
	}

	payload := makePayload(*payloadBytes)
	records, errorsByType, skippedPushes, skippedStaleAcks, duration, valid, fatalError, firstFailureSeq, firstFailureClient := runBenchmarkClients(
		*addr, *username, payload, scenario, *warmupMessages, *requestTimeout)

	summary := buildSummary(scenario, records, errorsByType, skippedPushes, skippedStaleAcks, duration,
		startedAt, time.Now().UTC(), valid, fatalError, firstFailureSeq, firstFailureClient)
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
		fmt.Printf("Fatal error at client %d seq %d: %s\n", summary.FirstFailureClient, summary.FirstFailureSeq, summary.FatalError)
	}
	if len(summary.Errors) > 0 {
		fmt.Printf("Errors: %s\n", formatErrorCounts(summary.Errors))
	}
	fmt.Printf("Success: %d/%d completed, requested=%d\n", summary.Success, summary.CompletedMessages, summary.RequestedMessages)
	fmt.Printf("Attempted QPS: %.2f, Success QPS: %.2f\n", summary.AttemptedQPS, summary.SuccessQPS)
	fmt.Printf("End-to-end QPS: attempted %.2f, success %.2f\n", summary.EndToEndAttemptedQPS, summary.EndToEndSuccessQPS)
	fmt.Printf("p50/p95/p99/p999 latency: %.3f / %.3f / %.3f / %.3f ms\n",
		summary.LatencyMS["p50"], summary.LatencyMS["p95"], summary.LatencyMS["p99"], summary.LatencyMS["p999"])
	fmt.Printf("Results written to %s\n", *outDir)
}

type clientResult struct {
	clientID         int
	records          []LatencyRecord
	errors           map[string]int
	skippedPushes    int
	skippedStaleAcks int
	valid            bool
	fatalError       string
	firstFailureSeq  uint64
}

func runBenchmarkClients(addr string, usernamePrefix string, payload []byte, scenario Scenario,
	warmupMessages int, requestTimeout time.Duration) ([]LatencyRecord, map[string]int, int, int, time.Duration, bool, string, uint64, int) {
	results := make(chan clientResult, scenario.Clients)
	online := make(chan struct{}, scenario.Clients)
	warmupDone := make(chan struct{}, scenario.Clients)
	warmupStart := make(chan struct{})
	start := make(chan struct{})
	receivers := &receiverRegistry{}
	var benchmarkDeadline atomic.Int64
	var wg sync.WaitGroup
	wg.Add(scenario.Clients)

	for clientID := 0; clientID < scenario.Clients; clientID++ {
		go func(clientID int) {
			defer wg.Done()
			results <- runOneBenchmarkClient(clientID, addr, benchmarkUsername(usernamePrefix, clientID, scenario.Clients), payload,
				scenario, warmupMessages, receivers, requestTimeout, online, warmupStart, warmupDone, start, &benchmarkDeadline)
		}(clientID)
	}

	for i := 0; i < scenario.Clients; i++ {
		<-online
	}
	close(warmupStart)
	for i := 0; i < scenario.Clients; i++ {
		<-warmupDone
	}
	benchStart := time.Now()
	if scenario.Mode == "rate_limited" {
		benchmarkDeadline.Store(benchStart.Add(time.Duration(scenario.DurationSeconds * float64(time.Second))).UnixNano())
	}
	close(start)
	wg.Wait()
	duration := time.Since(benchStart)
	close(results)

	allRecords := make([]LatencyRecord, 0, scenario.TotalMessages)
	errorsByType := map[string]int{}
	skippedPushes := 0
	skippedStaleAcks := 0
	valid := true
	fatalError := ""
	var firstFailureSeq uint64
	firstFailureClient := -1

	for result := range results {
		allRecords = append(allRecords, result.records...)
		for typ, count := range result.errors {
			errorsByType[typ] += count
		}
		skippedPushes += result.skippedPushes
		skippedStaleAcks += result.skippedStaleAcks
		if !result.valid {
			valid = false
			if fatalError == "" {
				fatalError = result.fatalError
				firstFailureSeq = result.firstFailureSeq
				firstFailureClient = result.clientID
			}
		}
	}

	sort.Slice(allRecords, func(i, j int) bool {
		if allRecords[i].SendUnix == allRecords[j].SendUnix {
			if allRecords[i].ClientID == allRecords[j].ClientID {
				return allRecords[i].Seq < allRecords[j].Seq
			}
			return allRecords[i].ClientID < allRecords[j].ClientID
		}
		return allRecords[i].SendUnix < allRecords[j].SendUnix
	})
	return allRecords, errorsByType, skippedPushes, skippedStaleAcks, duration, valid, fatalError, firstFailureSeq, firstFailureClient
}

func runOneBenchmarkClient(clientID int, addr string, username string, payload []byte, scenario Scenario,
	warmupMessages int, receivers *receiverRegistry, requestTimeout time.Duration, online chan<- struct{},
	warmupStart <-chan struct{}, warmupDone chan<- struct{}, start <-chan struct{}, benchmarkDeadline *atomic.Int64) clientResult {
	result := clientResult{
		clientID: clientID,
		errors:   map[string]int{},
		valid:    true,
	}

	if delay := connectRampDelay(clientID, scenario.Clients, time.Duration(scenario.ConnectRampSeconds*float64(time.Second))); delay > 0 {
		time.Sleep(delay)
	}

	conn, err := net.Dial("tcp", addr)
	if err != nil {
		result.valid = false
		result.fatalError = fmt.Sprintf("connect failed: %v", err)
		result.errors[classifyError(err)]++
		online <- struct{}{}
		warmupDone <- struct{}{}
		return result
	}
	var client *BenchClient
	defer func() {
		if client != nil && client.conn != nil {
			_ = client.conn.Close()
			return
		}
		_ = conn.Close()
	}()

	client = &BenchClient{
		clientID:       clientID,
		conn:           conn,
		rw:             bufio.NewReadWriter(bufio.NewReader(conn), bufio.NewWriter(conn)),
		nextSeq:        1,
		receiverID:     scenario.ReceiverID,
		receiverMode:   scenario.ReceiverMode,
		receivers:      receivers,
		requestTimeout: requestTimeout,
		rng:            rand.New(rand.NewSource(time.Now().UnixNano() + int64(clientID)*7919)),
	}
	if err := client.handshake(username); err != nil {
		result.valid = false
		result.fatalError = fmt.Sprintf("handshake failed: %v", err)
		result.errors[classifyError(err)]++
		online <- struct{}{}
		warmupDone <- struct{}{}
		return result
	}
	receivers.add(client.userID)
	client.waitForReceiverPeer(5 * time.Second)
	online <- struct{}{}

	if err := client.waitForStart(payload, warmupStart); err != nil {
		result.errors["prewarmup_"+classifyError(err)]++
		if reconnectErr := client.reconnect(addr, username); reconnectErr != nil {
			result.valid = false
			result.fatalError = fmt.Sprintf("pre-warmup keepalive failed and reconnect failed: keepalive=%v reconnect=%v", err, reconnectErr)
			result.errors[classifyError(reconnectErr)]++
			warmupDone <- struct{}{}
			return result
		}
	}
	if err := client.runWarmupP2P(payload, warmupMessages, scenario); err != nil {
		result.errors["warmup_"+classifyError(err)]++
		if reconnectErr := client.reconnect(addr, username); reconnectErr != nil {
			result.valid = false
			result.fatalError = fmt.Sprintf("warmup failed and reconnect failed: warmup=%v reconnect=%v", err, reconnectErr)
			result.errors[classifyError(reconnectErr)]++
			warmupDone <- struct{}{}
			return result
		}
	}

	warmupDone <- struct{}{}
	if err := client.waitForStart(payload, start); err != nil {
		result.errors["prestart_"+classifyError(err)]++
		if reconnectErr := client.reconnect(addr, username); reconnectErr != nil {
			result.valid = false
			result.fatalError = fmt.Sprintf("pre-start keepalive failed and reconnect failed: keepalive=%v reconnect=%v", err, reconnectErr)
			result.errors[classifyError(reconnectErr)]++
			return result
		}
	}
	deadline := time.Time{}
	if benchmarkDeadline != nil {
		if deadlineUnixNano := benchmarkDeadline.Load(); deadlineUnixNano > 0 {
			deadline = time.Unix(0, deadlineUnixNano)
		}
	}
	records, errorsByType, valid, fatalError, firstFailureSeq := client.runScenario(payload, scenario, deadline)
	for typ, count := range client.drainConnection(time.Duration(scenario.DrainSeconds * float64(time.Second))) {
		result.errors[typ] += count
	}
	for typ, count := range errorsByType {
		result.errors[typ] += count
	}
	result.records = records
	result.skippedPushes = client.skippedPushes
	result.skippedStaleAcks = client.skippedStaleAcks
	result.valid = result.valid && valid
	result.fatalError = fatalError
	result.firstFailureSeq = firstFailureSeq
	return result
}

func connectRampDelay(clientID int, clients int, ramp time.Duration) time.Duration {
	if ramp <= 0 || clients <= 1 {
		return 0
	}
	return time.Duration(float64(ramp) * float64(clientID) / float64(clients-1))
}

func benchmarkUsername(prefix string, clientID int, clients int) string {
	if clients == 1 {
		return prefix
	}
	return fmt.Sprintf("%s_%d", prefix, clientID)
}

func (c *BenchClient) next() uint64 {
	seq := c.nextSeq
	c.nextSeq++
	return seq
}

func (c *BenchClient) reconnect(addr string, username string) error {
	if c.conn != nil {
		_ = c.conn.Close()
	}
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		return err
	}
	c.conn = conn
	c.rw = bufio.NewReadWriter(bufio.NewReader(conn), bufio.NewWriter(conn))
	if err := c.handshake(username); err != nil {
		_ = conn.Close()
		return err
	}
	if c.receivers != nil {
		c.receivers.add(c.userID)
	}
	return nil
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
	if resp := registerResp.GetRegisterRes(); resp != nil && !resp.GetSuccess() && c.clientID == 0 {
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
	if resp := loginResp.GetLoginRes(); resp != nil && resp.GetUserInfo() != nil {
		c.userID = resp.GetUserInfo().GetUserId()
	}
	return nil
}

func (c *BenchClient) runScenario(content []byte, scenario Scenario, deadline time.Time) ([]LatencyRecord, map[string]int, bool, string, uint64) {
	if scenario.Mode == "rate_limited" {
		return c.runRateLimitedP2PBenchmark(content, time.Duration(scenario.DurationSeconds*float64(time.Second)),
			scenario.RatePerClient, scenario.RateSchedule, deadline)
	}
	return c.runP2PBenchmark(content, scenario.MessagesPerClient, scenario.Inflight)
}

func (c *BenchClient) waitForReceiverPeer(timeout time.Duration) {
	if c.receiverMode != "random-online" || c.receivers == nil {
		return
	}
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		for _, receiverID := range c.receivers.snapshot() {
			if receiverID != 0 && receiverID != c.userID {
				return
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func (c *BenchClient) waitForStart(content []byte, start <-chan struct{}) error {
	ticker := time.NewTicker(20 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-start:
			return nil
		case <-ticker.C:
			if _, err := c.sendMeasuredP2P(c.nextReceiverID(), content); err != nil {
				return err
			}
		}
	}
}

func (c *BenchClient) runWarmupP2P(content []byte, messages int, scenario Scenario) error {
	if messages <= 0 {
		return nil
	}

	interval := time.Duration(0)
	if scenario.Mode == "rate_limited" && scenario.RatePerClient > 0 {
		interval = time.Duration(float64(time.Second) / scenario.RatePerClient)
		if interval <= 0 {
			interval = time.Nanosecond
		}
	}

	for i := 0; i < messages; i++ {
		if interval > 0 {
			var delay time.Duration
			if scenario.RateSchedule == "poisson" {
				delay = time.Duration(c.rng.ExpFloat64() * float64(interval))
			} else if i == 0 {
				delay = time.Duration(c.rng.Float64() * float64(interval))
			} else {
				delay = interval
			}
			if maxDelay := 2 * interval; delay > maxDelay {
				delay = maxDelay
			}
			if delay > 0 {
				time.Sleep(delay)
			}
		}

		if _, err := c.sendMeasuredP2P(c.nextReceiverID(), content); err != nil {
			return fmt.Errorf("warmup failed at %d: %w", i, err)
		}
	}
	return nil
}

func (c *BenchClient) drainConnection(duration time.Duration) map[string]int {
	errorsByType := map[string]int{}
	if duration <= 0 || c.conn == nil {
		return errorsByType
	}
	defer c.conn.SetReadDeadline(time.Time{})

	deadline := time.Now().Add(duration)
	for time.Now().Before(deadline) {
		readWait := time.Second
		if remaining := time.Until(deadline); remaining < readWait {
			readWait = remaining
		}
		if readWait <= 0 {
			break
		}
		if err := c.conn.SetReadDeadline(time.Now().Add(readWait)); err != nil {
			errorsByType["drain_"+classifyError(err)]++
			return errorsByType
		}

		env, err := readPacket(c.rw)
		if err != nil {
			if classifyError(err) == "timeout" {
				continue
			}
			errorsByType["drain_"+classifyError(err)]++
			if isFatalConnectionError(err) {
				break
			}
			continue
		}
		if env.GetCmd() == pb.CommandType_CMD_MSG_ACK && env.GetSeq() != 0 {
			c.skippedStaleAcks++
			continue
		}
		c.skippedPushes++
	}
	return errorsByType
}

func (c *BenchClient) nextReceiverID() uint64 {
	if c.receiverMode != "random-online" || c.receivers == nil {
		return c.receiverID
	}

	ids := c.receivers.snapshot()
	if len(ids) == 0 {
		return c.receiverID
	}
	for i := 0; i < 8; i++ {
		receiverID := ids[c.rng.Intn(len(ids))]
		if receiverID != 0 && receiverID != c.userID {
			return receiverID
		}
	}
	for _, receiverID := range ids {
		if receiverID != 0 && receiverID != c.userID {
			return receiverID
		}
	}
	return c.receiverID
}

func (c *BenchClient) runRateLimitedP2PBenchmark(content []byte, duration time.Duration, ratePerClient float64,
	rateSchedule string, deadline time.Time) ([]LatencyRecord, map[string]int, bool, string, uint64) {
	records := make([]LatencyRecord, 0, int(duration.Seconds()*ratePerClient)+1)
	errorsByType := map[string]int{}
	valid := true
	fatalError := ""
	var firstFailureSeq uint64

	if duration <= 0 || ratePerClient <= 0 {
		return records, errorsByType, valid, fatalError, firstFailureSeq
	}

	interval := time.Duration(float64(time.Second) / ratePerClient)
	if interval <= 0 {
		interval = time.Nanosecond
	}
	if deadline.IsZero() {
		deadline = time.Now().Add(duration)
	}
	var nextSend time.Time
	if rateSchedule == "poisson" {
		nextSend = time.Now().Add(time.Duration(c.rng.ExpFloat64() * float64(interval)))
	} else {
		nextSend = time.Now().Add(time.Duration(c.rng.Float64() * float64(interval)))
	}

	for time.Now().Before(deadline) {
		now := time.Now()
		if now.Before(nextSend) {
			time.Sleep(nextSend.Sub(now))
		}
		if time.Now().After(deadline) {
			break
		}

		record, err := c.sendMeasuredP2P(c.nextReceiverID(), content)
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
		if rateSchedule == "poisson" {
			nextSend = nextSend.Add(time.Duration(c.rng.ExpFloat64() * float64(interval)))
		} else {
			nextSend = nextSend.Add(interval)
		}
	}
	return records, errorsByType, valid, fatalError, firstFailureSeq
}

func (c *BenchClient) runP2PBenchmark(content []byte, totalMessages int, inflight int) ([]LatencyRecord, map[string]int, bool, string, uint64) {
	records := make([]LatencyRecord, 0, totalMessages)
	errorsByType := map[string]int{}
	valid := true
	fatalError := ""
	var firstFailureSeq uint64

	if inflight == 1 {
		for i := 0; i < totalMessages; i++ {
			record, err := c.sendMeasuredP2P(c.nextReceiverID(), content)
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
		return records, errorsByType, valid, fatalError, firstFailureSeq
	}

	pending := make(map[uint64]int, inflight)
	sent := 0
	completed := 0
	for completed < totalMessages {
		for sent < totalMessages && len(pending) < inflight {
			record, err := c.sendP2PRequest(c.nextReceiverID(), content)
			records = append(records, record)
			pending[record.Seq] = len(records) - 1
			sent++
			if err != nil {
				errorsByType[classifyError(err)]++
				records[len(records)-1].Error = err.Error()
				records[len(records)-1].Success = false
				valid = false
				fatalError = err.Error()
				firstFailureSeq = record.Seq
				return records, errorsByType, valid, fatalError, firstFailureSeq
			}
		}

		env, err := c.readNextMessageAck()
		ackAt := time.Now()
		if err != nil {
			errorsByType[classifyError(err)]++
			valid = false
			fatalError = err.Error()
			for seq, idx := range pending {
				records[idx].AckUnix = ackAt.UnixNano()
				records[idx].LatencyUS = ackAt.Sub(time.Unix(0, records[idx].SendUnix)).Microseconds()
				records[idx].Error = err.Error()
				firstFailureSeq = seq
				break
			}
			return records, errorsByType, valid, fatalError, firstFailureSeq
		}

		idx, ok := pending[env.GetSeq()]
		if !ok {
			err := fmt.Errorf("unexpected ack seq=%d cmd=%s with no pending request", env.GetSeq(), env.GetCmd().String())
			errorsByType[classifyError(err)]++
			valid = false
			fatalError = err.Error()
			firstFailureSeq = env.GetSeq()
			return records, errorsByType, valid, fatalError, firstFailureSeq
		}
		delete(pending, env.GetSeq())
		completed++

		if err := applyMessageAck(&records[idx], env, ackAt); err != nil {
			errorsByType[classifyError(err)]++
			records[idx].Error = err.Error()
			records[idx].Success = false
			if isFatalConnectionError(err) {
				valid = false
				fatalError = err.Error()
				firstFailureSeq = records[idx].Seq
				return records, errorsByType, valid, fatalError, firstFailureSeq
			}
		}
	}
	return records, errorsByType, valid, fatalError, firstFailureSeq
}

func (c *BenchClient) sendP2PRequest(receiverID uint64, content []byte) (LatencyRecord, error) {
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

	record := LatencyRecord{ClientID: c.clientID, Seq: seq, SendUnix: sendAt.UnixNano()}
	if err := sendPacket(c.rw, env); err != nil {
		now := time.Now()
		record.AckUnix = now.UnixNano()
		record.LatencyUS = now.Sub(sendAt).Microseconds()
		return record, err
	}
	return record, nil
}

func (c *BenchClient) readNextMessageAck() (*pb.Envelope, error) {
	for {
		env, err := readPacket(c.rw)
		if err != nil {
			return nil, err
		}
		if env.GetCmd() == pb.CommandType_CMD_MSG_ACK && env.GetSeq() != 0 {
			return env, nil
		}
		if env.GetSeq() == 0 {
			c.skippedPushes++
			continue
		}
		return nil, fmt.Errorf("unexpected response while waiting for pipelined ack: seq=%d cmd=%s", env.GetSeq(), env.GetCmd().String())
	}
}

func applyMessageAck(record *LatencyRecord, respEnv *pb.Envelope, ackAt time.Time) error {
	record.AckUnix = ackAt.UnixNano()
	record.LatencyUS = ackAt.Sub(time.Unix(0, record.SendUnix)).Microseconds()
	ack := respEnv.GetMsgAck()
	if ack == nil {
		return fmt.Errorf("missing message ack")
	}
	record.MsgID = ack.GetMsgId()
	record.Status = ack.GetStatus().String()
	record.Success = ack.GetSuccess()
	if !ack.GetSuccess() {
		record.Error = ack.GetErrorMsg()
		return fmt.Errorf("message ack failed: %s", ack.GetErrorMsg())
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

	record := LatencyRecord{ClientID: c.clientID, Seq: seq, SendUnix: sendAt.UnixNano()}
	if err := sendPacket(c.rw, env); err != nil {
		record.AckUnix = time.Now().UnixNano()
		record.LatencyUS = time.Since(sendAt).Microseconds()
		return record, err
	}

	if c.requestTimeout > 0 && c.conn != nil {
		if err := c.conn.SetReadDeadline(time.Now().Add(c.requestTimeout)); err != nil {
			return record, err
		}
		defer c.conn.SetReadDeadline(time.Time{})
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
		if env.GetCmd() == cmd && env.GetSeq() < seq {
			c.skippedStaleAcks++
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

func buildSummary(scenario Scenario, records []LatencyRecord, errors map[string]int, skippedPushes int, skippedStaleAcks int,
	duration time.Duration, startedAt time.Time, finishedAt time.Time, valid bool, fatalError string,
	firstFailureSeq uint64, firstFailureClient int) Summary {
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
	measurementDuration := duration.Seconds()
	if scenario.Mode == "rate_limited" && scenario.DurationSeconds > 0 {
		measurementDuration = scenario.DurationSeconds
	}
	measurementMS := int64(measurementDuration * 1000)

	endToEndAttemptedQPS := 0.0
	if duration > 0 {
		endToEndAttemptedQPS = float64(len(records)) / duration.Seconds()
	}
	endToEndSuccessQPS := 0.0
	if success > 0 && duration > 0 {
		endToEndSuccessQPS = float64(success) / duration.Seconds()
	}

	attemptedQPS := 0.0
	if measurementDuration > 0 {
		attemptedQPS = float64(len(records)) / measurementDuration
	}
	successQPS := 0.0
	if success > 0 && scenario.Mode == "rate_limited" && measurementDuration > 0 {
		successQPS = float64(success) / measurementDuration
	} else if success > 0 && lastSuccessAck > firstSuccessSend {
		successWindowSeconds := float64(lastSuccessAck-firstSuccessSend) / float64(time.Second)
		successQPS = float64(success) / successWindowSeconds
	}
	return Summary{
		Scenario:             scenario.Name,
		Valid:                summaryValid(scenario, records, failed, valid),
		FatalError:           fatalError,
		FirstFailureSeq:      firstFailureSeq,
		FirstFailureClient:   firstFailureClient,
		Clients:              scenario.Clients,
		MessagesPerClient:    scenario.MessagesPerClient,
		Inflight:             scenario.Inflight,
		ConnectRampSeconds:   scenario.ConnectRampSeconds,
		DurationSeconds:      scenario.DurationSeconds,
		RatePerClient:        scenario.RatePerClient,
		RateSchedule:         scenario.RateSchedule,
		RequestTimeoutMS:     scenario.RequestTimeoutMS,
		DrainSeconds:         scenario.DrainSeconds,
		ReceiverMode:         scenario.ReceiverMode,
		Mode:                 scenario.Mode,
		RequestedMessages:    scenario.TotalMessages,
		CompletedMessages:    len(records),
		Success:              success,
		Failed:               failed,
		SkippedPushes:        skippedPushes,
		SkippedStaleAcks:     skippedStaleAcks,
		DurationMS:           duration.Milliseconds(),
		MeasurementMS:        measurementMS,
		AttemptedQPS:         attemptedQPS,
		SuccessQPS:           successQPS,
		EndToEndAttemptedQPS: endToEndAttemptedQPS,
		EndToEndSuccessQPS:   endToEndSuccessQPS,
		LatencyMS:            latencyStatsMS(latencies),
		Errors:               errors,
		StartedAt:            startedAt.Format(time.RFC3339Nano),
		FinishedAt:           finishedAt.Format(time.RFC3339Nano),
	}
}

func summaryValid(scenario Scenario, records []LatencyRecord, failed int, valid bool) bool {
	if !valid || failed != 0 {
		return false
	}
	if scenario.Mode == "rate_limited" {
		return len(records) > 0
	}
	return len(records) == scenario.TotalMessages
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

	if err := writer.Write([]string{"client_id", "seq", "msg_id", "send_unix_ns", "ack_unix_ns", "latency_us", "status", "success", "error"}); err != nil {
		return err
	}
	for _, record := range records {
		if err := writer.Write([]string{
			fmt.Sprintf("%d", record.ClientID),
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
- Clients: %d
- Mode: %s
- Messages per client: %d
- Target total messages: %d
- Connect ramp: %.3f seconds
- Duration: %.3f seconds
- Rate per client: %.3f msg/s
- Rate schedule: %s
- Request timeout: %d ms
- Drain: %.3f seconds
- Payload: %d bytes
- Inflight: %d
- Receiver mode: %s
- Pattern: %s

## Summary

- Success: %d/%d
- Failed: %d
- Valid: %t
- Fatal error: %s
- Errors: %s
- Measurement duration: %d ms
- End-to-end duration: %d ms
- Attempted QPS: %.2f
- Success QPS: %.2f
- End-to-end attempted QPS: %.2f
- End-to-end success QPS: %.2f
- p50: %.3f ms
- p95: %.3f ms
- p99: %.3f ms
- p999: %.3f ms
- Max: %.3f ms
- Skipped async pushes: %d
- Skipped stale ACKs: %d
`,
		scenario.Name,
		scenario.ServerAddr,
		scenario.Clients,
		scenario.Mode,
		scenario.MessagesPerClient,
		scenario.TotalMessages,
		scenario.ConnectRampSeconds,
		scenario.DurationSeconds,
		scenario.RatePerClient,
		scenario.RateSchedule,
		scenario.RequestTimeoutMS,
		scenario.DrainSeconds,
		scenario.PayloadBytes,
		scenario.Inflight,
		scenario.ReceiverMode,
		scenario.Pattern,
		summary.Success,
		summary.CompletedMessages,
		summary.Failed,
		summary.Valid,
		summary.FatalError,
		formatErrorCounts(summary.Errors),
		summary.MeasurementMS,
		summary.DurationMS,
		summary.AttemptedQPS,
		summary.SuccessQPS,
		summary.EndToEndAttemptedQPS,
		summary.EndToEndSuccessQPS,
		summary.LatencyMS["p50"],
		summary.LatencyMS["p95"],
		summary.LatencyMS["p99"],
		summary.LatencyMS["p999"],
		summary.LatencyMS["max"],
		summary.SkippedPushes,
		summary.SkippedStaleAcks,
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

func formatErrorCounts(errors map[string]int) string {
	if len(errors) == 0 {
		return "none"
	}
	keys := make([]string, 0, len(errors))
	for key := range errors {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	parts := make([]string, 0, len(keys))
	for _, key := range keys {
		parts = append(parts, fmt.Sprintf("%s=%d", key, errors[key]))
	}
	return strings.Join(parts, ", ")
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
	case strings.Contains(message, "unexpected response"):
		return "unexpected_response"
	case strings.Contains(message, "missing message ack"):
		return "missing_ack"
	case strings.Contains(message, "message too large"):
		return "message_too_large"
	case strings.Contains(message, "unmarshal failed"):
		return "unmarshal"
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
