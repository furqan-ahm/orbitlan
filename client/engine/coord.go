package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// Coordinator client: join a network, long-poll for membership + signaling, send signals, leave.

type MemberInfo struct {
	PeerID string `json:"peerID"`
	Name   string `json:"name"`
	IP     string `json:"ip"`
}

type joinResp struct {
	NetID       string       `json:"netID"`
	YourIP      string       `json:"yourIP"`
	Subnet      string       `json:"subnet"`
	Members     []MemberInfo `json:"members"`
	Version     int          `json:"version"`
	HostPeerID  string       `json:"hostPeerID"`
	IsHost      bool         `json:"isHost"`
	MemberCap   int          `json:"memberCap"`
	RoomEdition string       `json:"roomEdition"`
	Turn        *struct {
		URL        string `json:"url"`
		Username   string `json:"username"`
		Credential string `json:"credential"`
	} `json:"turn"`
}

type pollResp struct {
	Members  []MemberInfo `json:"members"`
	Messages []struct {
		From string          `json:"from"`
		Data json.RawMessage `json:"data"`
	} `json:"messages"`
	Version    int    `json:"version"`
	HostPeerID string `json:"hostPeerID"`
	IsHost     bool   `json:"isHost"`
}

type coordClient struct {
	base        string
	code        string
	peerID      string
	edition     string
	memberToken string
	http        *http.Client
}

type coordHTTPError struct {
	status  int
	message string
}

func (e *coordHTTPError) Error() string { return e.message }

func newCoord(base, code, peerID, edition, memberToken string) *coordClient {
	return &coordClient{
		base: base, code: code, peerID: peerID, edition: edition, memberToken: memberToken,
		http: &http.Client{Timeout: 40 * time.Second},
	}
}

func responseError(prefix string, resp *http.Response) error {
	detail, _ := io.ReadAll(io.LimitReader(resp.Body, 1024))
	message := strings.TrimSpace(string(detail))
	if message == "" {
		message = resp.Status
	}
	return &coordHTTPError{status: resp.StatusCode, message: prefix + ": " + message}
}

func (c *coordClient) join(name string) (*joinResp, error) {
	body, _ := json.Marshal(map[string]string{
		"code": c.code, "peerID": c.peerID, "name": name, "edition": c.edition,
		"memberToken": c.memberToken,
	})
	resp, err := c.http.Post(c.base+"/net/join", "application/json", bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, responseError("join", resp)
	}
	var jr joinResp
	if err := json.NewDecoder(resp.Body).Decode(&jr); err != nil {
		return nil, err
	}
	return &jr, nil
}

func (c *coordClient) signal(to string, data any) error {
	raw, _ := json.Marshal(data)
	body, _ := json.Marshal(map[string]any{
		"code": c.code, "from": c.peerID, "to": to, "data": json.RawMessage(raw),
	})
	req, _ := http.NewRequest("POST", c.base+"/net/signal", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-OrbitLan-Member", c.memberToken)
	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return responseError("signal", resp)
	}
	return nil
}

func (c *coordClient) poll(version int) (*pollResp, error) {
	url := fmt.Sprintf("%s/net/poll?code=%s&peerID=%s&version=%d", c.base, c.code, c.peerID, version)
	req, _ := http.NewRequest("GET", url, nil)
	req.Header.Set("X-OrbitLan-Member", c.memberToken)
	resp, err := c.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, responseError("poll", resp)
	}
	var pr pollResp
	if err := json.NewDecoder(resp.Body).Decode(&pr); err != nil {
		return nil, err
	}
	return &pr, nil
}

func (c *coordClient) leave() {
	body, _ := json.Marshal(map[string]string{"code": c.code, "peerID": c.peerID})
	req, _ := http.NewRequest("POST", c.base+"/net/leave", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-OrbitLan-Member", c.memberToken)
	client := &http.Client{Timeout: 5 * time.Second}
	if resp, err := client.Do(req); err == nil {
		resp.Body.Close()
	}
}

func (c *coordClient) kick(target string) error {
	body, _ := json.Marshal(map[string]string{
		"code": c.code, "from": c.peerID, "target": target, "memberToken": c.memberToken,
	})
	req, _ := http.NewRequest("POST", c.base+"/net/kick", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-OrbitLan-Member", c.memberToken)
	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return responseError("kick", resp)
	}
	return nil
}
