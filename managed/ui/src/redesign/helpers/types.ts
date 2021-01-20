export type DeepPartial<T> = T extends object ? { [P in keyof T]?: DeepPartial<T[P]> } : T;

export type ValidationErrors = Record<string, { message?: string } | undefined>;

export interface ControllerRenderProps<T> {
  value: T;
  onChange(value: T): void;
  onBlur(): void;
}
