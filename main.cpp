#include <stdio.h>
#include "pico/stdlib.h"

// 💡 FreeRTOS를 쓰기 위한 필수 헤더 2개
#include "FreeRTOS.h"
#include "task.h"

// 1. 우리가 만든 첫 번째 태스크 함수
void vHelloTask(void *pvParameters)
{
    while (true)
    {
        printf("Hello, FreeRTOS!\n");
        
        // 1초(1000ms) 동안 이 태스크를 쉬게 합니다.
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main()
{
    stdio_init_all(); // 시리얼 모니터 초기화

    // 2. FreeRTOS 커널에 태스크 등록하기
    xTaskCreate(
        vHelloTask,     // 실행할 태스크 함수 이름
        "HelloTask",    // 디버깅용 태스크 이름 (아무거나)
        256,            // 태스크가 쓸 메모리 크기 (스택)
        NULL,           // 태스크에 넘겨줄 매개변수 (없음)
        1,              // 태스크 우선순위 (1은 낮은 편)
        NULL            // 태스크 핸들 (지금은 필요 없음)
    );

    // 3. 🚀 스케줄러 시작! (컴퓨터에게 일을 시킵니다)
    vTaskStartScheduler();

    // 스케줄러가 켜지면 아래 코드는 실행되지 않습니다.
    while (true) {}
    return 0;
}

// ====================================================================================
// [추가] FreeRTOS 필수 Hook 함수 구현
// ====================================================================================
extern "C" {

// 1. 메모리 할당 실패(Malloc Failed) 시 호출되는 함수
void vApplicationMallocFailedHook(void)
{
    printf("Error: Malloc Failed!\n");
    while (true) {}
}

// 2. 태스크 스택 오버플로우 발생 시 호출되는 함수
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    printf("Error: Stack Overflow in task -> %s\n", pcTaskName);
    while (true) {}
}

} // extern "C"