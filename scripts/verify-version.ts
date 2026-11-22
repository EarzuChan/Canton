import { assertCantonVersionContract, readCantonVersionContract } from "./version-contract.ts"

const contract = readCantonVersionContract()
assertCantonVersionContract()
console.log('[CantonVersion]', `verified Canton version contract framework=${contract.frameworkVersion} frameworkProtocol=${contract.frameworkInternalProtocolCode} cli=${contract.cliVersion} cliCompatibility=${contract.cliCompatibility}`)
