# Operating System Projects

한양대학교 운영체제 수업(ELE3021, 2026) 프로젝트 모음입니다.
xv6 커널(RISC-V 기반)을 직접 수정하며 핵심 OS 개념을 구현했습니다.

## 프로젝트 목록

| 프로젝트 | 주제 | 설명 |
|---------|------|------|
| [Project 0](./project0_Syscall_Implementation) | Syscall Implementation | 시스템 콜 직접 구현 |
| [Project 1](./project1_CFS_scheduler) | CFS Scheduler | Linux CFS 스케줄링 알고리즘 구현 |
| [Project 2](./project2_Kernel_level_thread) | Kernel Level Thread | 커널 수준 스레드 구현 |
| [Project 3](./project3_Memory_mapped_files) | Memory Mapped Files | 메모리 매핑 파일 구현 |

## 개발 환경

- **OS**: xv6 (MIT 교육용 Unix 계열 OS)
- **아키텍처**: RISC-V
- **언어**: C
- **빌드**: Make

## 빌드 및 실행

```bash
make
make qemu
```
