# RDMA++ 2

I found that the original [RDMA++](https://github.com/howardlau1999/rdmapp) is not easy to use, so I made some changes to make it easier to use.

1. Resources are exposed via raw pointers. Use them carefully.
2. Allow you to pass any address and length to the send/recv verbs.

Mainly stripped out the original code, leaving only the core code, and added some comments.