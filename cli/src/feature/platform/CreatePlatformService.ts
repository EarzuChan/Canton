import type { Executor } from "./Executor.ts"
import type { PlatformService } from "./PlatformService.ts"
import { WindowsPlatformService } from "./WindowsPlatformService.ts"

export function createPlatformService(executor: Executor): PlatformService {
    return new WindowsPlatformService(executor)
}
