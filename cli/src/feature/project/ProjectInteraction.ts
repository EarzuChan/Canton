import type { CreateProjectRequest } from "./CreateProject.ts"
import type { InitializationPlan } from "./ProjectInitializer.ts"

export interface ProjectInteractionInput {
    readonly nodeRegistryUrl?: string
    readonly cmakeFetchContentUrl?: string
}

export interface ProjectInteraction {
    create(input: ProjectInteractionInput): Promise<false | CreateProjectRequest>
    confirmInitialization(plan: InitializationPlan, verb: string): Promise<boolean>
    confirm(message: string): Promise<boolean>
    message(message: string): void
    failure(message: string): void
}

export class ProjectInteractionCancelled extends Error { }
