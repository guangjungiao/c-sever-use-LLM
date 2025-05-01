python3 LLM.py
./http_server
wrk -t12 -c400 -d30s http://localhost:8080/