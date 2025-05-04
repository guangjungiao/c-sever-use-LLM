g++ -std=c++11 -O2 -pthread Server.cpp -o http_server -lhiredis -lcrypto
./http_server   