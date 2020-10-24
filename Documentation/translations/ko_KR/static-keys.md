===========
Static Keys
===========

.. 경고::

   폐지예정 API:

   'struct static_key' 를 직접사용하는 것은, 이제 구식이다. 따라서
   static_key_{true,false}()도 마찬가지이다. 예. 다음을 사용하지 마세요::

	struct static_key false = STATIC_KEY_INIT_FALSE;
	struct static_key true = STATIC_KEY_INIT_TRUE;
	static_key_true()
	static_key_false()

   업데이트되어 대체된 API는 다음과 같다::

	DEFINE_STATIC_KEY_TRUE(key);
	DEFINE_STATIC_KEY_FALSE(key);
	DEFINE_STATIC_KEY_ARRAY_TRUE(keys, count);
	DEFINE_STATIC_KEY_ARRAY_FALSE(keys, count);
	static_branch_likely()
	static_branch_unlikely()

요약
========

Static keys 는 GCC 기능과 코드 패칭 기술을 사용하여 성능에 민감한
fast-path 커널 코드에, 잘 사용하지 않는 기능을 포함할 수 있다.
간단예::
	DEFINE_STATIC_KEY_FALSE(key);

	...

        if (static_branch_unlikely(&key))
                do unlikely code
        else
                do likely code

	...
	static_branch_enable(&key);
	...
	static_branch_disable(&key);
	...
static_branch_unlikely() 매크로의 브랜치는 가능한한 likely 코드 경로에 적은
영향을 주는 코드에 만들어질 것이다.

동기
==========

현재, 추적점은 조건 branch로 구현된다. 조건 검사는 각 추적점의 전역
변수를 검사하여야 한다. 이 검사의 ovrhead 는 적지만 메모리 캐시가
압박을 받으면 증가한다 ( 전역변수에 대한 메모리 캐시 라인은 다른
메모리 접근과 공유될 수 있다 ). 커널의 추적점을 증가시키면 overhead는 
더 많은 문제가 될 것이다. 게다가 추적점은 종종 휴먼상태(비활성화됨)
이고 직접적인 커널 기능을 제공하지 않는다. 따라서 가능한 그 영향을 줄이는
것이 매우 바람직하다. 추적점이 이 작업의 원래 동기이긴 하지만 
다른 커널 코드 경로는 정적 키 기능을 사용할 수 있어야 한다.


해결책
========


gcc (v4.5)는 레이블로 분기할 수있는 새로운 'asm goto' 문을 추가한다.

http://gcc.gnu.org/ml/gcc-patches/2009-07/msg01556.html

'asm goto'문을 사용하여 메모리를 확인할 필요없이 기본으로 분기되어지거나 
그렇지 않은 브랜치를 만들 수 있다. 그런 다음 실행중에 분기
코드([1])를 패치하여 분기 방향을 바꿀 수 있다.

예를 들어 기본으로 비활성화된 단순 분기가 있다면::

	if (static_branch_unlikely(&key))
		printk("I am the true branch\n");

'printk'는 실행되지 않을 것이다. 그리고 생성된 코드는 단일 원자적 'no-op'
명령(x86에서 5byte)으로, 직선 코드 경로에 구성될 것이다. branch 가
반전 되었을 때 직선 코드 경로의 'no-op'은 라인 바깥의 true branch로
향하는 'jump' 명령으로 패치될 것이다. 그러므로 분기 방향 변경은 비용이
많이 들지만 분기 선택은 기본적으로 무료이다. 이것이 이 최적화의 일장일단이다.

이 저수준 패치 매커니즘을 'jump lable patching' 이라 부르고 static
keys 기능의 기초이다.

Static key label API, 사용법과 예
========================================


이 최적화를 사용하기 위해서 먼저 키를 정의하여야 한다::

	DEFINE_STATIC_KEY_TRUE(key);

또는::

	DEFINE_STATIC_KEY_FALSE(key);


키는 전역이어야 하고 즉, 스택이나 실행중에 동적으로 할당될 수 없다.
키는 코드에서 다음과 같이 사용 된다::

        if (static_branch_unlikely(&key))
                do unlikely code
        else
                do likely code

또는::

        if (static_branch_likely(&key))
                do likely code
        else
                do unlikely code
DEFINE_STATIC_KEY_TRUE(), 또는 DEFINE_STATIC_KEY_FALSE 로 정의된 키는, 
static_branch_likely() 나 static_branch_unlikely() 문으로 사용된다.

브랜치(들)은 다음을 통해 true로 설정될 수 있다::

	static_branch_enable(&key);

또는 다음을 통해 거짓으로::

	static_branch_disable(&key);

그런다음 브랜치(들)은 참조 counts 를 통해 전환 될 수 있다::

	static_branch_inc(&key);
	...
	static_branch_dec(&key);

그래서 적당한 참조 counting 과 함께 static_branch_inc()'는 
'true branch 만들기', 그리고 'static_branch_dec()' 는 
'false branch 만들기' 를 뜻한다. 예를 들면 키가 true로 초기화 되었다면
static_branch_dec()는 branch를 false 로 바꾼다. 그리고 그 뒤의
static_branch_inc()는 branch를 다시 true로 바꾼다. 똑같이 키가
false로 초기화 되었다면 'static_branch_inc()'는 branch를 true로
바꿀것이다. 그런다음 'static_branch_dec()'는 branch를 다시 false로
만든다

상태와 참조 카운트는 'static_key_enabled()'와 'static_key_count()'
함수로 검색할 수 있다. 보통 이러한 함수를 사용한다면 enable/disable
이나 increment/decrement 함수 주위에 사용된 동일한 mutex 로 보호되어야
한다.

분기 전환은 특히 CPU hotplug lock 같은 일부 잠금이 수행되는데 주의하라.
(커널이 패치되는 동안 커널로 가져오는 CPU들의 경쟁을 줄이기 위해)
hotplug 알림사이로부터 static key API부르기는 확실히 교착상태(deadlock)
레시피이다.
여전히 사용을 허용하기위해 다음 함수가 제공된다:
	static_key_enable_cpuslocked()
	static_key_disable_cpuslocked()
	static_branch_enable_cpuslocked()
	static_branch_disable_cpuslocked()

이 함수들은 일반 목적이 아니고 위의 문맥에 있다는 것을 실제로
알고 있을때만 사용되어야 하며 다른건([2]) 없다.

키 배열이 필요한 곳에는 다음처럼 정의 될 수 있다::

	DEFINE_STATIC_KEY_ARRAY_TRUE(keys, count);

또는::

	DEFINE_STATIC_KEY_ARRAY_FALSE(keys, count);

4) 아키텍쳐 레벨 코드 패칭 인터페이스, 'jump labels'


이 최적화를 활용하기위해 아키텍쳐에서 구현해야 할 몇가지 함수와
매크로가 있다. 만약 아키텍쳐 지원이 없다면 간단히 기존 load, test,
jump 순서로 대체될 것이다. 또한 jump_entry 테이블은 static_key->entry 
필드가 lsb 두개를 사용하므로 4-byte 정렬되어야 한다.

* ``select HAVE_ARCH_JUMP_LABEL``,
    see: arch/x86/Kconfig

* ``#define JUMP_LABEL_NOP_SIZE``,
    see: arch/x86/include/asm/jump_label.h

* ``__always_inline bool arch_static_branch(struct static_key *key, bool branch)``,
    see: arch/x86/include/asm/jump_label.h

* ``__always_inline bool arch_static_branch_jump(struct static_key *key, bool branch)``,
    see: arch/x86/include/asm/jump_label.h

* ``void arch_jump_label_transform(struct jump_entry *entry, enum jump_label_type type)``,
    see: arch/x86/kernel/jump_label.c

* ``__init_or_module void arch_jump_label_transform_static(struct jump_entry *entry, enum jump_label_type type)``,
    see: arch/x86/kernel/jump_label.c

* ``struct jump_entry``,
    see: arch/x86/include/asm/jump_label.h


5) Static keys / jump label 분석, 결과 (x86_64):


예를 들어 'getppid()'에 다음 branch를 추가하여, 이제 시스템 호출은 다음과 같다::

  SYSCALL_DEFINE0(getppid)
  {
        int pid;

  +     if (static_branch_unlikely(&key))
  +             printk("I am the true branch\n");

        rcu_read_lock();
        pid = task_tgid_vnr(rcu_dereference(current->real_parent));
        rcu_read_unlock();

        return pid;
  }

GCC에 의해 생성된 jump labels 있는 명령어 결과::

  ffffffff81044290 <sys_getppid>:
  ffffffff81044290:       55                      push   %rbp
  ffffffff81044291:       48 89 e5                mov    %rsp,%rbp
  ffffffff81044294:       e9 00 00 00 00          jmpq   ffffffff81044299 <sys_getppid+0x9>
  ffffffff81044299:       65 48 8b 04 25 c0 b6    mov    %gs:0xb6c0,%rax
  ffffffff810442a0:       00 00
  ffffffff810442a2:       48 8b 80 80 02 00 00    mov    0x280(%rax),%rax
  ffffffff810442a9:       48 8b 80 b0 02 00 00    mov    0x2b0(%rax),%rax
  ffffffff810442b0:       48 8b b8 e8 02 00 00    mov    0x2e8(%rax),%rdi
  ffffffff810442b7:       e8 f4 d9 00 00          callq  ffffffff81051cb0 <pid_vnr>
  ffffffff810442bc:       5d                      pop    %rbp
  ffffffff810442bd:       48 98                   cltq
  ffffffff810442bf:       c3                      retq
  ffffffff810442c0:       48 c7 c7 e3 54 98 81    mov    $0xffffffff819854e3,%rdi
  ffffffff810442c7:       31 c0                   xor    %eax,%eax
  ffffffff810442c9:       e8 71 13 6d 00          callq  ffffffff8171563f <printk>
  ffffffff810442ce:       eb c9                   jmp    ffffffff81044299 <sys_getppid+0x9>

jump label 최적화가 없으면 다음과 같이 보인다::

  ffffffff810441f0 <sys_getppid>:
  ffffffff810441f0:       8b 05 8a 52 d8 00       mov    0xd8528a(%rip),%eax        # ffffffff81dc9480 <key>
  ffffffff810441f6:       55                      push   %rbp
  ffffffff810441f7:       48 89 e5                mov    %rsp,%rbp
  ffffffff810441fa:       85 c0                   test   %eax,%eax
  ffffffff810441fc:       75 27                   jne    ffffffff81044225 <sys_getppid+0x35>
  ffffffff810441fe:       65 48 8b 04 25 c0 b6    mov    %gs:0xb6c0,%rax
  ffffffff81044205:       00 00
  ffffffff81044207:       48 8b 80 80 02 00 00    mov    0x280(%rax),%rax
  ffffffff8104420e:       48 8b 80 b0 02 00 00    mov    0x2b0(%rax),%rax
  ffffffff81044215:       48 8b b8 e8 02 00 00    mov    0x2e8(%rax),%rdi
  ffffffff8104421c:       e8 2f da 00 00          callq  ffffffff81051c50 <pid_vnr>
  ffffffff81044221:       5d                      pop    %rbp
  ffffffff81044222:       48 98                   cltq
  ffffffff81044224:       c3                      retq
  ffffffff81044225:       48 c7 c7 13 53 98 81    mov    $0xffffffff81985313,%rdi
  ffffffff8104422c:       31 c0                   xor    %eax,%eax
  ffffffff8104422e:       e8 60 0f 6d 00          callq  ffffffff81715193 <printk>
  ffffffff81044233:       eb c9                   jmp    ffffffff810441fe <sys_getppid+0xe>
  ffffffff81044235:       66 66 2e 0f 1f 84 00    data32 nopw %cs:0x0(%rax,%rax,1)
  ffffffff8104423c:       00 00 00 00

그래서 jump label이 비활성된 경우는 'mov', 'test' 와 'jne' 명령이
더해진 반면 jump label의 경우는 'no-op' 이나 'jmp 0' 만 가진다.
(jmp 0는 부팅시 5 byte 원자적 no-op 명령으로 패치된다.) 그래서, 
jump lable 이 비활성화된 경우는 다음이 더해진다 ::

  6 (mov) + 2 (test) + 2 (jne) = 10 - 5 (5 byte jump 0) = 5 추가 bytes.

패딩 bytes 까지 포함하면 jump label 코드는 이 작은 함수에 총 16 바이트
명령어 메모리를 절약한다. 이 경우 비 jump lable 함수는 80 바이트
길이이다. 따라서 20% 명령어 공간을 절약했다. 2-byte jmp 명령으로
브랜치에 도달할 수 있고 5-byte no-op은 사실 2-byte no-op 이 될 수 있으므로 
사실 이 부분은 더 개선될 수 있다. 그렇지만 아직 최적화된 no-op 크기는 
구현되지 않았다.(현재 하드코딩 됨)

scheduler 경로에서 많은 static key API를 사용하므로,
'pipe-test' ('perf bench sched pipe'라고도 함) 를 사용하여 
성능 향상을 보여줄 수 있다. 3.3.0-rc2 에서 수행된 테스트:

jump label 비활성화::

 'bash -c /tmp/pipe-test' 에 대한 성능 카운터 통계 (50회 실행):     

        855.700314 task-clock                #    0.534 CPUs utilized            ( +-  0.11% )
           200,003 context-switches          #    0.234 M/sec                    ( +-  0.00% )
                 0 CPU-migrations            #    0.000 M/sec                    ( +- 39.58% )
               487 page-faults               #    0.001 M/sec                    ( +-  0.02% )
     1,474,374,262 cycles                    #    1.723 GHz                      ( +-  0.17% )
   <not supported> stalled-cycles-frontend
   <not supported> stalled-cycles-backend
     1,178,049,567 instructions              #    0.80  insns per cycle          ( +-  0.06% )
       208,368,926 branches                  #  243.507 M/sec                    ( +-  0.06% )
         5,569,188 branch-misses             #    2.67% of all branches          ( +-  0.54% )

       1.601607384 seconds time elapsed                                          ( +-  0.07% )

jump label 활성화::

 'bash -c /tmp/pipe-test' 에 대한 성능 카운터 통계 (50회 실행):     

        841.043185 task-clock                #    0.533 CPUs utilized            ( +-  0.12% )
           200,004 context-switches          #    0.238 M/sec                    ( +-  0.00% )
                 0 CPU-migrations            #    0.000 M/sec                    ( +- 40.87% )
               487 page-faults               #    0.001 M/sec                    ( +-  0.05% )
     1,432,559,428 cycles                    #    1.703 GHz                      ( +-  0.18% )
   <not supported> stalled-cycles-frontend
   <not supported> stalled-cycles-backend
     1,175,363,994 instructions              #    0.82  insns per cycle          ( +-  0.04% )
       206,859,359 branches                  #  245.956 M/sec                    ( +-  0.04% )
         4,884,119 branch-misses             #    2.36% of all branches          ( +-  0.85% )

       1.579384366 초 경과

절약된 브랜치의 비율은 .7%이다. 그리고 12% '분기
누락(branch-misses)'을 절약하였다. 이 부분은 이 최적화가 
분기수의 감소에 대한 최적화이므로 가장 많은 절약을 기대한
지점이다. 
더하여 명령어의 .2%, cycles의 2.8%, 경과 시간의 1.4%를 절약했다.


역자주
========
1. patch the branch site 에서 site의 의미를 알 수 없어 소스 코드 분석
내용에 따라 코드의 의미로 해석했다.
2. and no other 해석이 안된다. 정확한 의미를 모르겠다.

Kernel/Documentation/static-keys.txt 
translated by ybgwon@gmail.com
