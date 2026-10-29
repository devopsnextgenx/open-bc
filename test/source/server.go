package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"time"
)

type User struct {
	ID        int    `json:"id"`
	Username  string `json:"username"`
	IsActive  bool   `json:"is_active"`
	CreatedAt string `json:"created_at"`
}

func main() {
	http.HandleFunc("/api/v1/status", HealthCheckHandler)
	http.HandleFunc("/api/v1/user", GetUserHandler)

	fmt.Println("Starting server on port :8080...")
	if err := http.ListenAndServe(":8080", nil); err != nil {
		panic(err)
	}
}

func HealthCheckHandler(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusOK)
	fmt.Fprintf(w, `{"status":"OK","timestamp":"%s"}`, time.Now().Format(time.RFC3339))
}

func GetUserHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}

	u := User{
		ID:        101,
		Username:  "gopher_dev",
		IsActive:  true,
		CreatedAt: "2026-10-01T12:00:00Z",
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(u)
}