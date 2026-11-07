{{- define "cartwheel.image" -}}
{{ .Values.image.repository }}:{{ .Values.image.tag }}
{{- end -}}

{{- define "cartwheel.kvSeeds" -}}
{{- $n := int .Values.kv.replicas -}}
{{- range $i := until $n -}}
{{- if $i }},{{ end -}}wheel-node-{{ $i }}=wheel-node-{{ $i }}.wheel-node:7100
{{- end -}}
{{- end -}}

{{- define "cartwheel.common.env" -}}
- name: CARTWHEEL_LOG_JSON
  value: "true"
- name: CARTWHEEL_TRACING_OTLP_ENDPOINT
  value: {{ .Values.tracing.otlpEndpoint | quote }}
{{- end -}}

{{- define "cartwheel.deal.env" -}}
- name: CARTWHEEL_POSTGRES_URL
  value: {{ .Values.infra.postgresUrl | quote }}
- name: CARTWHEEL_REDIS_URL
  value: {{ .Values.infra.redisUrl | quote }}
- name: CARTWHEEL_KAFKA_BROKERS
  value: {{ .Values.infra.kafkaBrokers | quote }}
- name: CARTWHEEL_KAFKA_REPLICATION
  value: "1"
{{- end -}}
