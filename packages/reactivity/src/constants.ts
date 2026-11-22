
export enum TrackOpTypes {
    GET = 'get',
    HAS = 'has',
    ITERATE = 'iterate',
}

export enum TriggerOpTypes {
    SET = 'set',
    ADD = 'add',
    DELETE = 'delete',
    CLEAR = 'clear',
}

export enum ReactiveFlags {
    SKIP = '__canton_skip',
    IS_REACTIVE = '__canton_isReactive',
    IS_READONLY = '__canton_isReadonly',
    IS_SHALLOW = '__canton_isShallow',
    RAW = '__canton_raw',
    IS_REF = '__canton_isRef',
}