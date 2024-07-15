# User-Level Thread Library

## Description
This project implements a user-level thread library in C++. 
It provides a set of functions for managing lightweight threads within a single process, 
offering basic concurrency capabilities without relying on operating system-level thread implementations.

## Features
- Thread creation and termination
- Thread blocking and resuming
- Thread sleeping
- Round-robin scheduling with timer-based preemption
- Management of thread states (READY, RUNNING, BLOCKED)
- Context switching between threads


## Purpose
This library is designed for educational purposes and applications requiring fine-grained control over thread management. It demonstrates low-level concepts of concurrent programming, including signal handling, context switching, and basic thread scheduling algorithms.

## Limitations
- Designed for single-process, multi-threaded environments
- Uses user-level threading, which may not take advantage of multiple CPU cores
