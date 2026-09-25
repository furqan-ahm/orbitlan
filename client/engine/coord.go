package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"time"
)

// Coordinator client: join a network, long-poll for membership + signaling, send signals, leave.

type MemberInfo struct {
	PeerID string `json:"peerID"`
	Name   string `json:"name"`
	IP     string `json:"ip"`
}

type joinResp struct {
	NetID   string       `json:"netID"`
	YourIP  string       `json:"yourIP"`
	Subnet  string       `json:"subnet"`
	Members []MemberInfo `json:"members"`
	Version int          `json:"version"`
	Turn    *struct {
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
	Version int `json:"version"`
}

type coordClient struct {
	base   string
	code   string
	peerID string
	http   *http.Client
}

func newCoord(base, code, peerID string) *coordClient {
	return &coordClient{
		base: base, code: code, peerID: peerID,
		http: &http.Client{Timeout: 40 * time.Second},
	}
}

func (c *coordClient) join(name string) (*joinResp, error) {
	body, _ := json.Marshal(map[string]string{"code": c.code, "peerID": c.peerID, "name": name})
	resp, err := c.http.Post(c.base+"/net/join", "application/json", bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("join: %s", resp.Status)
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
	resp, err := c.http.Post(c.base+"/net/signal", "application/json", bytes.NewReader(body))
	if err != nil {
		return err
	}
	resp.Body.Close()
	return nil
}

func (c *coordClient) poll(version int) (*pollResp, error) {
	url := fmt.Sprintf("%s/net/poll?code=%s&peerID=%s&version=%d", c.base, c.code, c.peerID, version)
	resp, err := c.http.Get(url)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("poll: %s", resp.Status)
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
	client := &http.Client{Timeout: 5 * time.Second}
	if resp, err := client.Do(req); err == nil {
		resp.Body.Close()
	}
}
