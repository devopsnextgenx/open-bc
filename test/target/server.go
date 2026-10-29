package main

import (
	"encoding/json"
	"log" // Changed from fmt to log
	"net/http"
	"time"
)

type Account struct { // Renamed struct from User
	ID        int    `json:"id"`
	Username  string `json:"username"`
	Enabled   bool   `json:"enabled"` // Renamed field from IsActive
	CreatedAt string `json:"created_at"`
}

func main() {
	// API routes inverted in order
	http.HandleFunc("/api/v1/user", GetUserHandler)
	http.HandleFunc("/api/v1/status", HealthCheckHandler)

	log.Println("Server is running on port :8080...")
	if err := http.ListenAndServe(":8080", nil); err != nil {
		log.Fatalf("Critical server error: %v", err) // Changed error handling style
	}
}

// Reordered: GetUserHandler is now placed above HealthCheckHandler
func GetUserHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" { // Changed from constant to hardcoded string
		http.Error(w, "Method Not Allowed", http.StatusMethodNotAllowed) // Changed response utility
		return
	}

	acc := Account{
		ID:        101,
		Username:  "gopher_dev",
		Enabled:   true,
		CreatedAt: "2026-10-01T12:00:00Z",
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(acc)
}

func HealthCheckHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json") // Added a missing header line
	w.WriteHeader(http.StatusOK)
	fmt.Fprintf(w, `{"status":"UP","timestamp":"%s"}`, time.Now().Format(time.RFC3339)) // Changed value "OK" to "UP"
}